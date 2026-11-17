/*
 * The BME280 driver (drivers/bme280.c) against a stand-in chip on a
 * stand-in I2C1: the 32-bit integer compensation against the data sheet's
 * floating-point formulas (section 8.1) and its worked example, the
 * calibration's packing, the start-up (ID, calibration, configuration
 * written asleep and read back), the minute's check finding a reset chip,
 * measurements not yet made, a chip that goes missing and one that is not a
 * BME280.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/bme280.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

// A calibration: T and P from the BMP280 data sheet's worked example, H
// typical of a BME280.
static const int32_t T1 = 27504, T2 = 26435, T3 = -1000;
static const int32_t P1 = 36477, P2 = -10685, P3 = 3024, P4 = 2855, P5 = 140, P6 = -7, P7 = 15500,
                     P8 = -14600, P9 = 6000;
static const int32_t H1 = 75, H2 = 362, H3 = 0, H4 = 324, H5 = 50, H6 = 30;

// --------------------------------------------------------------------------
// The data sheet's floating-point compensation, as the reference
// --------------------------------------------------------------------------

static double reference_t_fine(double adc_t)
{
    double var1 = (adc_t / 16384.0 - T1 / 1024.0) * T2;
    double var2 = (adc_t / 131072.0 - T1 / 8192.0) * (adc_t / 131072.0 - T1 / 8192.0) * T3;

    return var1 + var2;
}


static double reference_pressure(double adc_p, double t_fine)
{
    double var1 = t_fine / 2.0 - 64000.0;
    double var2 = var1 * var1 * P6 / 32768.0;
    double p;

    var2 = var2 + var1 * P5 * 2.0;
    var2 = var2 / 4.0 + P4 * 65536.0;
    var1 = (P3 * var1 * var1 / 524288.0 + P2 * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * P1;
    p = 1048576.0 - adc_p;
    p = (p - var2 / 4096.0) * 6250.0 / var1;
    var1 = P9 * p * p / 2147483648.0;
    var2 = p * P8 / 32768.0;
    return p + (var1 + var2 + P7) / 16.0;
}


static double reference_humidity(double adc_h, double t_fine)
{
    double h = t_fine - 76800.0;

    h = (adc_h - (H4 * 64.0 + H5 / 16384.0 * h)) *
        (H2 / 65536.0 * (1.0 + H6 / 67108864.0 * h * (1.0 + H3 / 67108864.0 * h)));
    h = h * (1.0 - H1 * h / 524288.0);
    return h < 0.0 ? 0.0 : h > 100.0 ? 100.0 : h;
}

// --------------------------------------------------------------------------
// The chip: 256 registers, written in register/value pairs, read with the
// address incrementing.
// --------------------------------------------------------------------------

static uint8_t regs[256];
static bool answering = true;
static int writes, reads, checks_read, id_reads, ignored_config;

static void put16(uint8_t *at, int32_t value)
{
    at[0] = (uint8_t)(value & 0xFF);
    at[1] = (uint8_t)((value >> 8) & 0xFF);
}


static void chip_calibration(void)
{
    uint8_t *a = &regs[BME280_REG_CALIB_A];
    uint8_t *b = &regs[BME280_REG_CALIB_B];

    put16(&a[0], T1);
    put16(&a[2], T2);
    put16(&a[4], T3);
    put16(&a[6], P1);
    put16(&a[8], P2);
    put16(&a[10], P3);
    put16(&a[12], P4);
    put16(&a[14], P5);
    put16(&a[16], P6);
    put16(&a[18], P7);
    put16(&a[20], P8);
    put16(&a[22], P9);
    a[25] = (uint8_t)H1;
    put16(&b[0], H2);
    b[2] = (uint8_t)H3;
    b[3] = (uint8_t)((H4 >> 4) & 0xFF);
    b[4] = (uint8_t)((H4 & 0x0F) | ((H5 & 0x0F) << 4));
    b[5] = (uint8_t)((H5 >> 4) & 0xFF);
    b[6] = (uint8_t)H6;
}


// What a reset leaves: asleep, nothing measured.
static void chip_reset(void)
{
    regs[BME280_REG_CTRL_HUM] = 0;
    regs[BME280_REG_CTRL_MEAS] = 0;
    regs[BME280_REG_CONFIG] = 0;
    memcpy(&regs[BME280_REG_DATA], "\x80\x00\x00\x80\x00\x00\x80\x00", 8);
}


static void chip_data(uint32_t adc_p, uint32_t adc_t, uint32_t adc_h)
{
    uint8_t *d = &regs[BME280_REG_DATA];

    d[0] = (uint8_t)(adc_p >> 12);
    d[1] = (uint8_t)(adc_p >> 4);
    d[2] = (uint8_t)(adc_p << 4);
    d[3] = (uint8_t)(adc_t >> 12);
    d[4] = (uint8_t)(adc_t >> 4);
    d[5] = (uint8_t)(adc_t << 4);
    d[6] = (uint8_t)(adc_h >> 8);
    d[7] = (uint8_t)adc_h;
}


bool i2c1_submit(I2cTransfer *transfer)
{
    CHECK(transfer->address == BME280_ADDRESS);
    if (!answering)
    {
        transfer->result = I2C_NACK_ADDRESS;
        return true;
    }
    if (transfer->read_length == 0U)
    {
        CHECK(transfer->write_length % 2U == 0U);
        for (uint8_t index = 0; index + 1U < transfer->write_length; index += 2U)
        {
            uint8_t reg = transfer->write[index];
            uint8_t value = transfer->write[index + 1U];

            // The data sheet: config writes in normal mode may be ignored.
            if (reg == BME280_REG_CONFIG && (regs[BME280_REG_CTRL_MEAS] & 0x03U) != 0U)
            {
                ignored_config++;
                continue;
            }
            regs[reg] = value;
        }
        writes++;
    }
    else
    {
        uint8_t reg = transfer->write[0];

        id_reads += reg == BME280_REG_ID;
        checks_read += reg == BME280_REG_CTRL_HUM;
        reads += reg == BME280_REG_DATA;
        for (uint8_t index = 0; index < transfer->read_length; index++)
        {
            transfer->read[index] = regs[(uint8_t)(reg + index)];
        }
    }
    transfer->result = I2C_OK;
    return true;
}


bool i2c1_done(const I2cTransfer *transfer)
{
    return transfer->result != I2C_QUEUED && transfer->result != I2C_BUSY;
}


const char *i2c1_result_name(I2cResult result)
{
    return result == I2C_OK ? "ok" : "no answer";
}


static uint32_t now = 1;

static void run_ms(uint32_t ms)
{
    while (ms-- > 0)
    {
        (void)bme280_service(now++);
    }
}


static void test_compensation(void)
{
    Bme280Calibration cal;
    Bme280Reading reading;
    double worst_t = 0.0, worst_p = 0.0, worst_h = 0.0;

    printf("Compensation: 32-bit integer against the data sheet's floating point\n");
    chip_calibration();
    bme280_decode_calibration(&regs[BME280_REG_CALIB_A], &regs[BME280_REG_CALIB_B], &cal);
    CHECK(cal.t1 == T1 && cal.t2 == T2 && cal.t3 == T3 && cal.p1 == P1 && cal.p2 == P2 &&
          cal.p9 == P9 && cal.h1 == H1 && cal.h2 == H2 && cal.h3 == H3 && cal.h4 == H4 &&
          cal.h5 == H5 && cal.h6 == H6);
    {
        // Negative H4 and H5: the 12-bit fields are signed.
        uint8_t b[BME280_CALIB_B_BYTES] = {0, 0, 0, 0xFE, 0xD3, 0xFF, 0};
        Bme280Calibration negative;

        bme280_decode_calibration(&regs[BME280_REG_CALIB_A], b, &negative);
        CHECK(negative.h4 == -29 && negative.h5 == -3);
    }

    // The worked example: 519888 is 25.08 C, and 415148 about 100653 Pa
    // (the 32-bit formula gives 100656).
    bme280_compensate(&cal, 519888UL, 415148UL, 30000UL, &reading);
    CHECK(reading.temperature_valid && reading.temperature_centic == 2508);
    CHECK(reading.pressure_valid && labs((long)reading.pressure_pa - 100653L) <= 4);
    printf("  example: %ld centi-C, %lu Pa\n", (long)reading.temperature_centic,
           (unsigned long)reading.pressure_pa);

    // A sweep: about -10..+60 C, 300..1100 hPa, the humidity across its range.
    for (uint32_t adc_t = 440000UL; adc_t <= 640000UL; adc_t += 4000UL)
    {
        for (uint32_t adc_p = 250000UL; adc_p <= 550000UL; adc_p += 15000UL)
        {
            for (uint32_t adc_h = 20000UL; adc_h <= 50000UL; adc_h += 3000UL)
            {
                double t_fine = reference_t_fine((double)adc_t);
                double t = t_fine / 5120.0;
                double p = reference_pressure((double)adc_p, t_fine);
                double h = reference_humidity((double)adc_h, t_fine);

                bme280_compensate(&cal, adc_t, adc_p, adc_h, &reading);
                if (!reading.temperature_valid)
                {
                    continue;
                }
                if (fabs(reading.temperature_centic / 100.0 - t) > worst_t)
                {
                    worst_t = fabs(reading.temperature_centic / 100.0 - t);
                }
                if (p >= 30000.0 && p <= 110000.0)
                {
                    CHECK(reading.pressure_valid);
                    if (fabs((double)reading.pressure_pa - p) > worst_p)
                    {
                        worst_p = fabs((double)reading.pressure_pa - p);
                    }
                }
                CHECK(reading.humidity_valid);
                if (fabs(reading.humidity_centipercent / 100.0 - h) > worst_h)
                {
                    worst_h = fabs(reading.humidity_centipercent / 100.0 - h);
                }
            }
        }
    }
    printf("  worst differences: %.4f C, %.2f Pa, %.4f %%RH\n", worst_t, worst_p, worst_h);
    CHECK(worst_t <= 0.011);
    CHECK(worst_p <= 8.0); // the 32-bit formula: a few Pa, 0.08 hPa at most
    CHECK(worst_h <= 0.02);

    // Not measured yet: nothing valid; pressure and humidity skipped alone.
    bme280_compensate(&cal, BME280_SKIPPED_20, 415148UL, 30000UL, &reading);
    CHECK(!reading.temperature_valid && !reading.pressure_valid && !reading.humidity_valid);
    bme280_compensate(&cal, 519888UL, BME280_SKIPPED_20, BME280_SKIPPED_16, &reading);
    CHECK(reading.temperature_valid && !reading.pressure_valid && !reading.humidity_valid);
}


static void test_driver(void)
{
    const Bme280Status *status = bme280_status();

    printf("Driver: found, calibrated, configured asleep, read\n");
    memset(regs, 0, sizeof(regs));
    regs[BME280_REG_ID] = BME280_CHIP_ID;
    chip_calibration();
    chip_reset();
    bme280_init();
    run_ms(5);
    CHECK(id_reads == 1 && status->present && status->identified && status->calibrated);
    CHECK(writes == 1 && checks_read == 1 && status->configured && status->writes == 1);
    CHECK(regs[BME280_REG_CTRL_HUM] == 0x01 && regs[BME280_REG_CTRL_MEAS] == 0xAF &&
          regs[BME280_REG_CONFIG] == 0xA8 && ignored_config == 0);

    // The first read before a measurement: nothing yet.
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    CHECK(reads == 1 && !status->reading.temperature_valid && !bme280_report_due());
    chip_data(415148UL, 519888UL, 30000UL);
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    CHECK(status->reading.temperature_valid && status->reading.temperature_centic == 2508 &&
          status->reading.pressure_valid && status->reading.humidity_valid);
    CHECK(bme280_report_due() && !bme280_report_due()); // the first measurement, once
    bme280_print(now, true);

    // Reset without our noticing: the minute's check finds it asleep and
    // writes the configuration again; reads meanwhile are not used.
    printf("Driver: reset found by the minute's check\n");
    chip_reset();
    regs[BME280_REG_CTRL_MEAS] = 0x27; // as if left in normal mode, other settings
    writes = 0;
    run_ms(BME280_CHECK_INTERVAL_MS + 2U * BME280_READ_INTERVAL_MS + 2U);
    CHECK(writes == 1 && status->configured && status->mismatches == 1 &&
          regs[BME280_REG_CONFIG] == 0xA8 && ignored_config == 0);
    chip_data(415148UL, 519888UL, 30000UL);
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    CHECK(status->reading.temperature_valid && status->reading.temperature_centic == 2508);

    // Missing, then back from power-on: found, calibrated and configured
    // again before a reading is used.
    printf("Driver: missing, then back\n");
    answering = false;
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    CHECK(!status->present && !status->configured && !status->reading.temperature_valid);
    bme280_print(now, false);
    chip_reset();
    answering = true;
    writes = 0;
    id_reads = 0;
    run_ms(BME280_DETECT_INTERVAL_MS + 2U);
    CHECK(id_reads == 1 && writes == 1 && status->configured);
    chip_data(415148UL, 519888UL, 30000UL);
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    CHECK(status->reading.temperature_valid);

    // Paused for the RTC source: no reads.
    bme280_set_auto_read(false);
    reads = 0;
    run_ms(3U * BME280_READ_INTERVAL_MS);
    CHECK(reads == 0);
    bme280_set_auto_read(true);

    // Something else at the address: never configured.
    printf("Driver: not a BME280\n");
    answering = false;
    run_ms(BME280_READ_INTERVAL_MS + 1U);
    answering = true;
    regs[BME280_REG_ID] = 0x58; // a BMP280
    writes = 0;
    run_ms(3U * BME280_DETECT_INTERVAL_MS);
    CHECK(status->present && !status->identified && status->chip_id == 0x58 && writes == 0 &&
          !status->reading.temperature_valid);
    bme280_print(now, false);
}


int main(void)
{
    test_compensation();
    test_driver();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
