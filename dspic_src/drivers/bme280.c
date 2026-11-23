#include "bme280.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

static Bme280Status status;
static Bme280Calibration calibration;
static uint32_t last_attempt_ms;
static uint32_t last_check_ms;
static bool report_next; // the next completed read is worth reporting
static bool report_due;  // ... and has completed
static bool auto_read = true;
static I2cTransfer *pending; // the transfer of the request in progress

static uint8_t id_address = BME280_REG_ID;
static uint8_t id_data;
static I2cTransfer id_transfer = I2C_TRANSFER(BME280_ADDRESS, &id_address, 1U, &id_data, 1U);

static uint8_t calib_a_address = BME280_REG_CALIB_A;
static uint8_t calib_a[BME280_CALIB_A_BYTES];
static I2cTransfer calib_a_transfer =
    I2C_TRANSFER(BME280_ADDRESS, &calib_a_address, 1U, calib_a, BME280_CALIB_A_BYTES);
static uint8_t calib_b_address = BME280_REG_CALIB_B;
static uint8_t calib_b[BME280_CALIB_B_BYTES];
static I2cTransfer calib_b_transfer =
    I2C_TRANSFER(BME280_ADDRESS, &calib_b_address, 1U, calib_b, BME280_CALIB_B_BYTES);

// Register and value pairs, in order: asleep, then humidity, the config
// (ignored in normal mode), then measuring.
static uint8_t write_buffer[8] = {BME280_REG_CTRL_MEAS,  BME280_MODE_SLEEP,     BME280_REG_CTRL_HUM,
                                  BME280_CTRL_HUM_VALUE, BME280_REG_CONFIG,     BME280_CONFIG_VALUE,
                                  BME280_REG_CTRL_MEAS,  BME280_CTRL_MEAS_VALUE};
static I2cTransfer write_transfer =
    I2C_TRANSFER(BME280_ADDRESS, write_buffer, sizeof(write_buffer), 0, 0U);

static uint8_t ctrl_address = BME280_REG_CTRL_HUM;
static uint8_t ctrl_data[BME280_CTRL_BYTES];
static I2cTransfer check_transfer =
    I2C_TRANSFER(BME280_ADDRESS, &ctrl_address, 1U, ctrl_data, BME280_CTRL_BYTES);

static uint8_t data_address = BME280_REG_DATA;
static uint8_t data[BME280_DATA_BYTES];
static I2cTransfer read_transfer =
    I2C_TRANSFER(BME280_ADDRESS, &data_address, 1U, data, BME280_DATA_BYTES);

// --------------------------------------------------------------------------
// Compensation (data sheet section 4.2.3 and 8.2, 32-bit)
// --------------------------------------------------------------------------

static int32_t u16le(const uint8_t *bytes)
{
    return (int32_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}


static int32_t s16le(const uint8_t *bytes)
{
    return (int32_t)(int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}


void bme280_decode_calibration(const uint8_t a[BME280_CALIB_A_BYTES],
                               const uint8_t b[BME280_CALIB_B_BYTES], Bme280Calibration *cal)
{
    cal->t1 = u16le(&a[0]);
    cal->t2 = s16le(&a[2]);
    cal->t3 = s16le(&a[4]);
    cal->p1 = u16le(&a[6]);
    cal->p2 = s16le(&a[8]);
    cal->p3 = s16le(&a[10]);
    cal->p4 = s16le(&a[12]);
    cal->p5 = s16le(&a[14]);
    cal->p6 = s16le(&a[16]);
    cal->p7 = s16le(&a[18]);
    cal->p8 = s16le(&a[20]);
    cal->p9 = s16le(&a[22]);
    cal->h1 = (int32_t)a[25];
    cal->h2 = s16le(&b[0]);
    cal->h3 = (int32_t)b[2];
    // 0xE4 [11:4], 0xE5 [3:0]; 0xE6 [11:4], 0xE5 [7:4]: signed 12 bits.
    cal->h4 = (int32_t)(int8_t)b[3] * 16 + (int32_t)(b[4] & 0x0FU);
    cal->h5 = (int32_t)(int8_t)b[5] * 16 + (int32_t)(b[4] >> 4);
    cal->h6 = (int32_t)(int8_t)b[6];
}


// 0.01 C, and t_fine for the others.
static int32_t compensate_temperature(const Bme280Calibration *cal, int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = (((adc_t >> 3) - cal->t1 * 2) * cal->t2) >> 11;
    int32_t var2 = (adc_t >> 4) - cal->t1;

    var2 = (((var2 * var2) >> 12) * cal->t3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}


// Pa; 0 if the calibration would divide by zero.
static uint32_t compensate_pressure(const Bme280Calibration *cal, int32_t adc_p, int32_t t_fine)
{
    int32_t var1 = (t_fine >> 1) - (int32_t)64000;
    int32_t var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * cal->p6;
    uint32_t p;

    var2 = var2 + var1 * cal->p5 * 2;
    var2 = (var2 >> 2) + cal->p4 * (int32_t)65536;
    var1 = (((cal->p3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) + ((cal->p2 * var1) >> 1)) >> 18;
    var1 = (((int32_t)32768 + var1) * cal->p1) >> 15;
    if (var1 == 0)
    {
        return 0U;
    }
    p = ((uint32_t)((int32_t)1048576 - adc_p) - (uint32_t)(var2 >> 12)) * (uint32_t)3125;
    if (p < (uint32_t)0x80000000UL)
    {
        p = (p << 1) / (uint32_t)var1;
    }
    else
    {
        p = (p / (uint32_t)var1) * 2U;
    }
    var1 = (cal->p9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
    var2 = ((int32_t)(p >> 2) * cal->p8) >> 13;
    return (uint32_t)((int32_t)p + ((var1 + var2 + cal->p7) >> 4));
}


// %RH in Q22.10.
static uint32_t compensate_humidity(const Bme280Calibration *cal, int32_t adc_h, int32_t t_fine)
{
    int32_t x = t_fine - (int32_t)76800;

    x = ((((adc_h * (int32_t)16384) - (cal->h4 * (int32_t)1048576) - (cal->h5 * x)) +
          (int32_t)16384) >>
         15) *
        (((((((x * cal->h6) >> 10) * (((x * cal->h3) >> 11) + (int32_t)32768)) >> 10) +
           (int32_t)2097152) *
              cal->h2 +
          (int32_t)8192) >>
         14);
    x = x - (((((x >> 15) * (x >> 15)) >> 7) * cal->h1) >> 4);
    if (x < 0)
    {
        x = 0;
    }
    if (x > (int32_t)419430400)
    {
        x = (int32_t)419430400;
    }
    return (uint32_t)(x >> 12);
}


void bme280_compensate(const Bme280Calibration *cal, uint32_t raw_temperature,
                       uint32_t raw_pressure, uint32_t raw_humidity, Bme280Reading *reading)
{
    int32_t t_fine;
    uint32_t value;

    reading->temperature_valid = false;
    reading->pressure_valid = false;
    reading->humidity_valid = false;
    if (raw_temperature == BME280_SKIPPED_20)
    {
        return;
    }
    reading->temperature_centic = compensate_temperature(cal, (int32_t)raw_temperature, &t_fine);
    reading->temperature_valid =
        reading->temperature_centic >= -4000L && reading->temperature_centic <= 8500L;
    if (!reading->temperature_valid)
    {
        return;
    }
    if (raw_pressure != BME280_SKIPPED_20)
    {
        value = compensate_pressure(cal, (int32_t)raw_pressure, t_fine);
        reading->pressure_pa = value;
        reading->pressure_valid = value >= 30000UL && value <= 110000UL;
    }
    if (raw_humidity != BME280_SKIPPED_16)
    {
        // Q22.10 to 0.01 %RH, rounded.
        value = (compensate_humidity(cal, (int32_t)raw_humidity, t_fine) * (uint32_t)100 +
                 (uint32_t)512) >>
                10;
        reading->humidity_centipercent =
            (uint16_t)(value > (uint32_t)10000 ? (uint32_t)10000 : value);
        reading->humidity_valid = true;
    }
}


// --------------------------------------------------------------------------
// The requests
// --------------------------------------------------------------------------

static void decode_data(void)
{
    status.raw_pressure =
        ((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | ((uint32_t)data[2] >> 4);
    status.raw_temperature =
        ((uint32_t)data[3] << 12) | ((uint32_t)data[4] << 4) | ((uint32_t)data[5] >> 4);
    status.raw_humidity = ((uint32_t)data[6] << 8) | (uint32_t)data[7];
    bme280_compensate(&calibration, status.raw_temperature, status.raw_pressure,
                      status.raw_humidity, &status.reading);
}


static bool start(I2cTransfer *transfer, Bme280Request request)
{
    if (!i2c1_submit(transfer))
    {
        return false;
    }
    status.request = request;
    pending = transfer;
    return true;
}


static bool busy(void)
{
    return status.request != BME280_NONE && status.request != BME280_DONE &&
           status.request != BME280_FAILED;
}


static void invalidate(void)
{
    status.reading.temperature_valid = false;
    status.reading.pressure_valid = false;
    status.reading.humidity_valid = false;
}


void bme280_init(void)
{
    report_next = start(&id_transfer, BME280_IDENTIFYING);
}


// The configuration read back: as written, or to be written again. ctrl_hum
// bits 2:0, ctrl_meas all, config without bit 1 (reserved).
static void finish_check(uint32_t now_ms)
{
    bool same = (ctrl_data[0] & 0x07U) == BME280_CTRL_HUM_VALUE &&
                ctrl_data[2] == BME280_CTRL_MEAS_VALUE &&
                (ctrl_data[3] & 0xFDU) == BME280_CONFIG_VALUE;

    last_check_ms = now_ms;
    if (!same && status.configured)
    {
        status.mismatches++;
    }
    status.configured = same;
    if (!same)
    {
        invalidate();
    }
}


// A transfer ended: true if it was a read of the data.
static bool finish(I2cTransfer *transfer, uint32_t now_ms)
{
    I2cResult result = transfer->result;

    status.last_result = result;
    last_attempt_ms = now_ms;
    if (result == I2C_NACK_ADDRESS)
    {
        status.present = false;
        status.identified = false;
        status.calibrated = false;
        status.configured = false;
        invalidate();
    }
    else if (result == I2C_OK || result == I2C_NACK_DATA)
    {
        status.present = true;
    }
    if (result != I2C_OK)
    {
        status.failures++;
        status.request = BME280_FAILED;
        return transfer == &read_transfer;
    }
    status.request = BME280_DONE;
    if (transfer == &id_transfer)
    {
        status.chip_id = id_data;
        status.identified = id_data == BME280_CHIP_ID;
        if (status.identified)
        {
            (void)start(&calib_a_transfer, BME280_CALIBRATING);
        }
        return false;
    }
    if (transfer == &calib_a_transfer)
    {
        if (!start(&calib_b_transfer, BME280_CALIBRATING))
        {
            status.request = BME280_FAILED;
        }
        return false;
    }
    if (transfer == &calib_b_transfer)
    {
        bme280_decode_calibration(calib_a, calib_b, &calibration);
        status.calibrated = true;
        status.configured = false;
        if (start(&write_transfer, BME280_WRITING))
        {
            status.writes++;
        }
        return false;
    }
    if (transfer == &write_transfer)
    {
        // Read back what the chip now holds.
        if (!start(&check_transfer, BME280_CHECKING))
        {
            status.request = BME280_FAILED;
        }
        return false;
    }
    if (transfer == &check_transfer)
    {
        finish_check(now_ms);
        return false;
    }
    status.reads++;
    status.read_ms = now_ms;
    if (!status.configured)
    {
        // Measured with whatever the chip holds: not used.
        invalidate();
        return false;
    }
    decode_data();
    return true;
}


// The next request, when one is due: identify a chip not known, calibrate
// and configure, check a configured one, else read the data.
static void next_request(uint32_t now_ms)
{
    last_attempt_ms = now_ms;
    if (!status.present || !status.identified)
    {
        (void)start(&id_transfer, BME280_IDENTIFYING);
    }
    else if (!status.calibrated)
    {
        (void)start(&calib_a_transfer, BME280_CALIBRATING);
    }
    else if (!status.configured)
    {
        if (start(&write_transfer, BME280_WRITING))
        {
            status.writes++;
        }
    }
    else if (now_ms - last_check_ms >= BME280_CHECK_INTERVAL_MS)
    {
        (void)start(&check_transfer, BME280_CHECKING);
    }
    else
    {
        (void)start(&read_transfer, BME280_READING);
    }
}


bool bme280_service(uint32_t now_ms)
{
    bool read_done = false;

    if (busy() && i2c1_done(pending))
    {
        read_done = finish(pending, now_ms);
    }
    if (read_done && report_next && status.configured && status.reading.temperature_valid)
    {
        report_next = false;
        report_due = true;
    }
    if (auto_read && !busy() &&
        now_ms - last_attempt_ms >= (status.present && status.identified
                                         ? BME280_READ_INTERVAL_MS
                                         : BME280_DETECT_INTERVAL_MS))
    {
        next_request(now_ms);
    }
    return read_done;
}


bool bme280_report_due(void)
{
    bool due = report_due;

    report_due = false;
    return due;
}


void bme280_set_auto_read(bool enabled)
{
    auto_read = enabled;
}


const Bme280Status *bme280_status(void)
{
    return &status;
}


// 0.01 units as d.dd, signed.
static void print_centi(int32_t value)
{
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);

    printf("%s%lu.%02lu", value < 0 ? "-" : "", (unsigned long)(magnitude / 100UL),
           (unsigned long)(magnitude % 100UL));
}


void bme280_print(uint32_t now_ms, bool verbose)
{
    const Bme280Reading *reading = &status.reading;

    printf("Env: BME280, ");
    if (!status.present)
    {
        printf("%s\r\n", status.request == BME280_NONE ? "not read yet" : "NOT FOUND");
        return;
    }
    if (!status.identified)
    {
        printf("UNKNOWN chip ID 0x%02X", status.chip_id);
    }
    else if (!status.configured || status.reads == 0U || !reading->temperature_valid)
    {
        printf("%s", !status.calibrated  ? "not calibrated yet"
                     : status.configured ? "not read yet"
                                         : "not configured yet");
    }
    else
    {
        printf("board ");
        print_centi(reading->temperature_centic);
        printf(" C (ambient ~");
        print_centi(reading->temperature_centic + BME280_AMBIENT_OFFSET_CENTIC);
        printf(" C)");
        if (reading->pressure_valid)
        {
            printf(", %lu.%02lu hPa", (unsigned long)(reading->pressure_pa / 100UL),
                   (unsigned long)(reading->pressure_pa % 100UL));
        }
        if (reading->humidity_valid)
        {
            printf(", ");
            print_centi(reading->humidity_centipercent);
            printf(" %%RH");
        }
        printf(", %lu s ago", (unsigned long)((now_ms - status.read_ms) / 1000UL));
    }
    if (status.request == BME280_FAILED)
    {
        printf(", FAILED: %s", i2c1_result_name(status.last_result));
    }
    if (verbose)
    {
        printf("; raw T 0x%05lX P 0x%05lX H 0x%04lX, %lu reads, %lu writes, %lu reconfigured, "
               "%lu failures",
               (unsigned long)status.raw_temperature, (unsigned long)status.raw_pressure,
               (unsigned long)status.raw_humidity, (unsigned long)status.reads,
               (unsigned long)status.writes, (unsigned long)status.mismatches,
               (unsigned long)status.failures);
    }
    printf("\r\n");
}
