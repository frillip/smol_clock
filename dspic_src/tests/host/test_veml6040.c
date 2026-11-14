/*
 * The VEML6040 driver (drivers/veml6040.c) against a stand-in I2C1: the
 * configuration written when the chip is found and read back, checked every
 * minute and after the chip went missing, lux from the green count at
 * 320 ms, saturation and darkness.
 */
#include <stdio.h>
#include <string.h>

#include "../../drivers/veml6040.h"

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

// The bus: transfers finish at once, as the test says.
static bool answering = true;
static uint16_t green = 1000;
static uint8_t configuration = 0xFF; // what the chip holds
static int writes, reads, checks_read;

bool i2c1_submit(I2cTransfer *transfer)
{
    if (!answering)
    {
        transfer->result = I2C_NACK_ADDRESS;
        return true;
    }
    if (transfer->write_length == 3U)
    {
        CHECK(transfer->address == VEML6040_ADDRESS && transfer->write[0] == VEML6040_CMD_CONF &&
              transfer->write[2] == 0U);
        configuration = transfer->write[1];
        writes++;
    }
    else if (transfer->write[0] == VEML6040_CMD_CONF)
    {
        CHECK(transfer->read_length == 2U);
        transfer->read[0] = configuration;
        transfer->read[1] = 0U;
        checks_read++;
    }
    else
    {
        CHECK(transfer->write[0] == VEML6040_CMD_G_DATA && transfer->read_length == 2U);
        transfer->read[0] = (uint8_t)green;
        transfer->read[1] = (uint8_t)(green >> 8);
        reads++;
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
        (void)veml6040_service(now++);
    }
}


int main(void)
{
    const Veml6040Status *status = veml6040_status();

    veml6040_init();
    run_ms(2);
    CHECK(writes == 1 && checks_read == 1 && configuration == VEML6040_CONF_IT_320MS &&
          status->configured);
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(reads >= 1 && status->present && status->lux_valid);
    CHECK(status->lux_x10 == 315);                          // 1000 x 0.03146 lux = 31.5 lux
    CHECK(veml6040_report_due() && !veml6040_report_due()); // the first read, once

    green = 0xFFFF;
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(status->saturated && !status->lux_valid && status->lux_x10 == 20617);
    CHECK(!veml6040_report_due());
    green = 0;
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(status->too_dark && !status->lux_valid);

    // Reset without our noticing (no failed transfer): the minute's check
    // finds the default and rewrites it; counts meanwhile are not used.
    configuration = 0x00;
    green = 1000;
    writes = 0;
    run_ms(VEML6040_CHECK_INTERVAL_MS + 2U * VEML6040_READ_INTERVAL_MS + 2U);
    CHECK(writes == 1 && configuration == VEML6040_CONF_IT_320MS && status->configured &&
          status->mismatches == 1);
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(status->lux_valid && status->lux_x10 == 315);

    // Missing, then back with its power-on configuration: written again
    // before a count is used.
    answering = false;
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(!status->present && !status->configured && !status->lux_valid);
    configuration = 0x00; // power-on default: 40 ms
    answering = true;
    green = 1000;
    writes = 0;
    run_ms(VEML6040_DETECT_INTERVAL_MS + VEML6040_READ_INTERVAL_MS + 2U);
    CHECK(writes == 1 && configuration == VEML6040_CONF_IT_320MS && status->configured);
    run_ms(VEML6040_READ_INTERVAL_MS + 1U);
    CHECK(status->lux_valid && status->lux_x10 == 315);

    // Paused for the RTC source: no reads.
    veml6040_set_auto_read(false);
    reads = 0;
    run_ms(3U * VEML6040_READ_INTERVAL_MS);
    CHECK(reads == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
