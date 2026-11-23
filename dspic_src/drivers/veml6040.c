#include "veml6040.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#define DATA_BYTES      2U
#define SENSITIVITY_DIV 10000UL // 1/100000 lux to 0.1 lux
#define FULL_SCALE      0xFFFFU

static const uint32_t sensitivity[] = {VEML6040_SENSITIVITY_40MS,  VEML6040_SENSITIVITY_80MS,
                                       VEML6040_SENSITIVITY_160MS, VEML6040_SENSITIVITY_320MS,
                                       VEML6040_SENSITIVITY_640MS, VEML6040_SENSITIVITY_1280MS};

static Veml6040Status status;
static uint32_t last_attempt_ms;
static uint32_t last_check_ms;
static bool report_next; // the next completed read is worth reporting
static bool report_due;  // ... and has completed
static bool auto_read = true;

static uint8_t read_address = VEML6040_CMD_G_DATA;
static uint8_t data[DATA_BYTES];
static uint8_t write_buffer[3];
static I2cTransfer read_transfer =
    I2C_TRANSFER(VEML6040_ADDRESS, &read_address, 1U, data, DATA_BYTES);
static I2cTransfer write_transfer =
    I2C_TRANSFER(VEML6040_ADDRESS, write_buffer, sizeof(write_buffer), 0, 0U);
static uint8_t conf_address = VEML6040_CMD_CONF;
static uint8_t conf_data[DATA_BYTES];
static I2cTransfer check_transfer =
    I2C_TRANSFER(VEML6040_ADDRESS, &conf_address, 1U, conf_data, DATA_BYTES);

// The green count, little-endian, to lux.
static void decode(void)
{
    uint8_t index = (uint8_t)((status.configuration & VEML6040_CONF_IT_MASK) >> 4);
    uint32_t lux_x10;

    status.green = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    lux_x10 =
        ((uint32_t)status.green * sensitivity[index] + SENSITIVITY_DIV / 2U) / SENSITIVITY_DIV;
    status.saturated = status.green == FULL_SCALE || lux_x10 > UINT16_MAX;
    status.too_dark = status.green == 0U;
    status.lux_valid = !status.saturated && !status.too_dark;
    status.lux_x10 = lux_x10 > UINT16_MAX ? UINT16_MAX : (uint16_t)lux_x10;
}


static bool start(I2cTransfer *transfer, Veml6040Request request)
{
    if (!i2c1_submit(transfer))
    {
        return false;
    }
    status.request = request;
    return true;
}


static bool busy(void)
{
    return status.request == VEML6040_READING || status.request == VEML6040_WRITING ||
           status.request == VEML6040_CHECKING;
}


// The configuration register, low byte then high (reserved, 0).
static bool configure(void)
{
    status.configuration = (uint8_t)(VEML6040_CONF_SD_ENABLE | VEML6040_CONF_AF_AUTO |
                                     VEML6040_CONF_TRIG_DISABLE | VEML6040_INTEGRATION);
    write_buffer[0] = VEML6040_CMD_CONF;
    write_buffer[1] = status.configuration;
    write_buffer[2] = 0U;
    return start(&write_transfer, VEML6040_WRITING);
}


void veml6040_init(void)
{
    status.configuration = VEML6040_INTEGRATION;
    report_next = configure();
}


// The configuration read back: as written, or to be written again.
static void finish_check(uint32_t now_ms)
{
    bool same = conf_data[0] == status.configuration && conf_data[1] == 0U;

    last_check_ms = now_ms;
    if (!same && status.configured)
    {
        status.mismatches++;
    }
    status.configured = same;
}


// A transfer ended: true if it was a read of the light.
static bool finish(const I2cTransfer *transfer, uint32_t now_ms)
{
    I2cResult result = transfer->result;

    status.last_result = result;
    last_attempt_ms = now_ms;
    if (result == I2C_NACK_ADDRESS)
    {
        status.present = false;
        status.configured = false;
        status.lux_valid = false;
    }
    else if (result == I2C_OK || result == I2C_NACK_DATA)
    {
        status.present = true;
    }
    if (result != I2C_OK)
    {
        status.failures++;
        status.request = VEML6040_FAILED;
        return transfer == &read_transfer;
    }
    status.request = VEML6040_DONE;
    if (transfer == &write_transfer)
    {
        // Read back what the chip now holds.
        status.writes++;
        if (!start(&check_transfer, VEML6040_CHECKING))
        {
            status.request = VEML6040_FAILED;
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
        status.lux_valid = false;
        return false;
    }
    decode();
    return true;
}


// The next request, when one is due: configure a chip not known to be
// configured, check one that is, else read the light.
static void next_request(uint32_t now_ms)
{
    last_attempt_ms = now_ms;
    if (status.present && !status.configured)
    {
        (void)configure();
    }
    else if (status.present && now_ms - last_check_ms >= VEML6040_CHECK_INTERVAL_MS)
    {
        (void)start(&check_transfer, VEML6040_CHECKING);
    }
    else
    {
        (void)start(&read_transfer, VEML6040_READING);
    }
}


bool veml6040_service(uint32_t now_ms)
{
    bool read_done = false;

    if (status.request == VEML6040_READING && i2c1_done(&read_transfer))
    {
        read_done = finish(&read_transfer, now_ms);
    }
    else if (status.request == VEML6040_WRITING && i2c1_done(&write_transfer))
    {
        read_done = finish(&write_transfer, now_ms);
    }
    else if (status.request == VEML6040_CHECKING && i2c1_done(&check_transfer))
    {
        read_done = finish(&check_transfer, now_ms);
    }
    if (read_done && report_next && status.configured)
    {
        report_next = false;
        report_due = true;
    }
    if (auto_read && !busy() &&
        now_ms - last_attempt_ms >=
            (status.present ? VEML6040_READ_INTERVAL_MS : VEML6040_DETECT_INTERVAL_MS))
    {
        next_request(now_ms);
    }
    return read_done;
}


bool veml6040_report_due(void)
{
    bool due = report_due;

    report_due = false;
    return due;
}


void veml6040_set_auto_read(bool enabled)
{
    auto_read = enabled;
}


const Veml6040Status *veml6040_status(void)
{
    return &status;
}


void veml6040_print(uint32_t now_ms, bool verbose)
{
    printf("ALS: VEML6040, ");
    if (!status.present)
    {
        printf("%s\r\n", status.request == VEML6040_NONE ? "not read yet" : "NOT FOUND");
        return;
    }
    if (!status.configured || status.reads == 0U)
    {
        printf("%s", status.configured ? "not read yet" : "not configured yet");
    }
    else
    {
        printf("%u.%u lux%s%s, %lu s ago", status.lux_x10 / 10U, status.lux_x10 % 10U,
               status.saturated ? " SATURATED" : "", status.too_dark ? " TOO DARK" : "",
               (unsigned long)((now_ms - status.read_ms) / 1000UL));
    }
    if (status.request == VEML6040_FAILED)
    {
        printf(", FAILED: %s", i2c1_result_name(status.last_result));
    }
    if (verbose)
    {
        printf("; green 0x%04X, %lu reads, %lu writes, %lu reconfigured, %lu failures",
               status.green, (unsigned long)status.reads, (unsigned long)status.writes,
               (unsigned long)status.mismatches, (unsigned long)status.failures);
    }
    printf("\r\n");
}
