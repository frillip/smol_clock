/*
 * PCF8563 real-time clock. See pcf8563.h.
 *
 * Both requests are single I2C transfers from control/status 1: a read
 * writes the register address and reads nine registers back; a write sends
 * the address and nine registers. The chip's address pointer increments by
 * itself.
 */
#include "pcf8563.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "../platform/utc.h"

#define REGISTER_COUNT 9U

static Pcf8563Status status;
static uint8_t read_address = PCF8563_CONTROL_STATUS_1;
static uint8_t write_buffer[1U + REGISTER_COUNT];
static I2cTransfer read_transfer =
    I2C_TRANSFER(PCF8563_ADDRESS, &read_address, 1U, status.registers, REGISTER_COUNT);
static I2cTransfer write_transfer =
    I2C_TRANSFER(PCF8563_ADDRESS, write_buffer, sizeof(write_buffer), 0, 0U);
static const uint8_t release_buffer[2] = {PCF8563_CONTROL_STATUS_1, 0x00U};
static I2cTransfer release_transfer =
    I2C_TRANSFER(PCF8563_ADDRESS, release_buffer, sizeof(release_buffer), 0, 0U);
static uint32_t last_attempt_ms;
static bool report_next; // the next completed read is worth reporting
static bool report_due;  // ... and has completed
static bool auto_read = true;

uint8_t pcf8563_bcd_to_binary(uint8_t bcd)
{
    return (uint8_t)(bcd - 6U * (bcd >> 4));
}


uint8_t pcf8563_binary_to_bcd(uint8_t binary)
{
    return (uint8_t)(binary + 6U * (binary / 10U));
}


static bool bcd_valid(uint8_t bcd)
{
    return (bcd & 0x0FU) <= 9U && (bcd >> 4) <= 9U;
}


// The registers as last read: flags, and the time if it is to be believed.
static void decode(void)
{
    const uint8_t *r = status.registers;
    uint8_t seconds = r[PCF8563_VL_SECONDS] & 0x7FU;
    uint8_t minutes = r[PCF8563_MINUTES] & 0x7FU;
    uint8_t hours = r[PCF8563_HOURS] & 0x3FU;
    uint8_t days = r[PCF8563_DAYS] & 0x3FU;
    uint8_t months = r[PCF8563_CENTURY_MONTHS] & 0x1FU;
    uint8_t years = r[PCF8563_YEARS];
    UtcCivil civil;

    status.voltage_low = (r[PCF8563_VL_SECONDS] & PCF8563_VL) != 0U;
    status.stopped = (r[PCF8563_CONTROL_STATUS_1] & PCF8563_STOP) != 0U;
    status.century = (r[PCF8563_CENTURY_MONTHS] & PCF8563_CENTURY) != 0U;
    status.calendar_valid = false;
    status.time_valid = false;
    if (!bcd_valid(seconds) || !bcd_valid(minutes) || !bcd_valid(hours) || !bcd_valid(days) ||
        !bcd_valid(months) || !bcd_valid(years))
    {
        return;
    }
    civil.year = (uint16_t)(2000U + pcf8563_bcd_to_binary(years) + (status.century ? 100U : 0U));
    civil.month = pcf8563_bcd_to_binary(months);
    civil.day = pcf8563_bcd_to_binary(days);
    civil.hour = pcf8563_bcd_to_binary(hours);
    civil.minute = pcf8563_bcd_to_binary(minutes);
    civil.second = pcf8563_bcd_to_binary(seconds);
    // The chip counts 0-59: a 60 is not a leap second, it is nonsense.
    if (civil.second > 59U || !utc_civil_is_valid(&civil))
    {
        return;
    }
    status.utc_seconds = utc_from_civil(&civil);
    status.calendar_valid = true;
    status.time_valid = !status.voltage_low && !status.stopped;
}


static bool start(I2cTransfer *transfer, Pcf8563Request request)
{
    if (!i2c1_submit(transfer))
    {
        return false;
    }
    status.request = request;
    return true;
}


void pcf8563_init(uint32_t now_ms)
{
    status.request = PCF8563_NONE;
    status.last_result = I2C_IDLE;
    last_attempt_ms = now_ms;
    report_next = true;
    (void)pcf8563_read();
}


bool pcf8563_busy(void)
{
    return status.request == PCF8563_READING || status.request == PCF8563_WRITING ||
           status.request == PCF8563_WRITING_STOPPED || status.request == PCF8563_RELEASING;
}


bool pcf8563_read(void)
{
    if (pcf8563_busy())
    {
        return false;
    }
    return start(&read_transfer, PCF8563_READING);
}


// The ten bytes from control/status 1: the time, and control as given.
static void encode(uint32_t utc_seconds, uint8_t control_1)
{
    UtcCivil civil;
    uint16_t days = (uint16_t)(utc_seconds / UTC_SECONDS_PER_DAY);
    uint8_t *r = &write_buffer[1];

    utc_to_civil(utc_seconds, false, &civil);
    write_buffer[0] = PCF8563_CONTROL_STATUS_1;
    r[PCF8563_CONTROL_STATUS_1] = control_1;                     // STOP as given, TESTC clear
    r[PCF8563_CONTROL_STATUS_2] = 0x00U;                         // no alarm or timer interrupts
    r[PCF8563_VL_SECONDS] = pcf8563_binary_to_bcd(civil.second); // VL cleared
    r[PCF8563_MINUTES] = pcf8563_binary_to_bcd(civil.minute);
    r[PCF8563_HOURS] = pcf8563_binary_to_bcd(civil.hour);
    r[PCF8563_DAYS] = pcf8563_binary_to_bcd(civil.day);
    r[PCF8563_WEEKDAYS] = (uint8_t)((days + 4U) % 7U); // 0 Sunday: 1970-01-01 was a Thursday
    r[PCF8563_CENTURY_MONTHS] = (uint8_t)(pcf8563_binary_to_bcd(civil.month) |
                                          (civil.year >= 2100U ? PCF8563_CENTURY : 0U));
    r[PCF8563_YEARS] = pcf8563_binary_to_bcd((uint8_t)(civil.year % 100U));
}


bool pcf8563_write(uint32_t utc_seconds)
{
    if (pcf8563_busy() || utc_seconds < PCF8563_FIRST_UTC)
    {
        return false;
    }
    encode(utc_seconds, 0x00U); // running
    if (!start(&write_transfer, PCF8563_WRITING))
    {
        return false;
    }
    report_next = true;
    return true;
}


bool pcf8563_write_stopped(uint32_t utc_seconds)
{
    if (pcf8563_busy() || utc_seconds < PCF8563_FIRST_UTC)
    {
        return false;
    }
    encode(utc_seconds, PCF8563_STOP);
    return start(&write_transfer, PCF8563_WRITING_STOPPED);
}


bool pcf8563_release(void)
{
    if (pcf8563_busy())
    {
        return false;
    }
    return start(&release_transfer, PCF8563_RELEASING);
}


// A transfer ended: true if it was the read.
static bool finish(const I2cTransfer *transfer, uint32_t now_ms)
{
    I2cResult result = transfer->result;

    status.last_result = result;
    last_attempt_ms = now_ms;
    if (result == I2C_NACK_ADDRESS)
    {
        status.present = false;
        status.calendar_valid = false;
        status.time_valid = false;
    }
    else if (result == I2C_OK || result == I2C_NACK_DATA)
    {
        status.present = true;
    }
    if (result != I2C_OK)
    {
        status.failures++;
        status.request = PCF8563_FAILED;
        return transfer == &read_transfer;
    }
    if (status.request == PCF8563_WRITING_STOPPED || status.request == PCF8563_RELEASING)
    {
        // The aligned write reads for itself, once the chip runs.
        if (status.request == PCF8563_WRITING_STOPPED)
        {
            status.writes++;
        }
        else
        {
            status.releases++;
        }
        status.request = PCF8563_DONE;
        return false;
    }
    if (transfer == &write_transfer)
    {
        status.writes++;
        // Read back what the chip now holds.
        status.request = PCF8563_DONE;
        if (!start(&read_transfer, PCF8563_READING))
        {
            status.request = PCF8563_FAILED;
        }
        return false;
    }
    status.reads++;
    status.read_ms = now_ms;
    status.read_stamp = read_transfer.start_stamp;
    status.request = PCF8563_DONE;
    decode();
    return true;
}


bool pcf8563_service(uint32_t now_ms)
{
    bool read_done = false;

    if (status.request == PCF8563_READING && i2c1_done(&read_transfer))
    {
        read_done = finish(&read_transfer, now_ms);
    }
    else if ((status.request == PCF8563_WRITING || status.request == PCF8563_WRITING_STOPPED) &&
             i2c1_done(&write_transfer))
    {
        read_done = finish(&write_transfer, now_ms);
    }
    else if (status.request == PCF8563_RELEASING && i2c1_done(&release_transfer))
    {
        read_done = finish(&release_transfer, now_ms);
    }
    if (read_done && report_next)
    {
        report_next = false;
        report_due = true;
    }
    if (auto_read && !pcf8563_busy() &&
        now_ms - last_attempt_ms >=
            (status.present ? PCF8563_READ_INTERVAL_MS : PCF8563_DETECT_INTERVAL_MS))
    {
        last_attempt_ms = now_ms;
        (void)pcf8563_read();
    }
    return read_done;
}


bool pcf8563_report_due(void)
{
    bool due = report_due;

    report_due = false;
    return due;
}


void pcf8563_set_auto_read(bool enabled)
{
    auto_read = enabled;
}


const Pcf8563Status *pcf8563_status(void)
{
    return &status;
}


bool pcf8563_now(uint32_t now_ms, uint32_t *utc_seconds)
{
    if (!status.calendar_valid)
    {
        return false;
    }
    // Where in its second the read fell is unknown: half way on average,
    // so the elapsed time is rounded rather than truncated.
    *utc_seconds = status.utc_seconds + (now_ms - status.read_ms + 500UL) / 1000UL;
    return true;
}


void pcf8563_print(uint32_t now_ms, Pcf8563Reference reference, bool verbose)
{
    uint32_t rtc_now;
    uint32_t age_s = (now_ms - status.read_ms) / 1000UL;

    printf("RTC: PCF8563, ");
    if (!status.present)
    {
        printf("%s\n", status.request == PCF8563_NONE ? "not read yet" : "NOT FOUND");
        return;
    }
    if (status.reads == 0U)
    {
        printf("not read yet");
    }
    else if (!pcf8563_now(now_ms, &rtc_now))
    {
        printf("NO VALID DATE");
        if (verbose)
        {
            printf(" (registers %02X %02X %02X %02X %02X %02X %02X)", status.registers[2],
                   status.registers[3], status.registers[4], status.registers[5],
                   status.registers[6], status.registers[7], status.registers[8]);
        }
    }
    else
    {
        utc_print(rtc_now);
        if (reference.known)
        {
            printf(", %+ld s", (long)((int32_t)(rtc_now - reference.utc_seconds)));
        }
        if (status.voltage_low)
        {
            printf(", VL (power lost)");
        }
        else if (!status.time_valid)
        {
            printf(", NOT VALID");
        }
        if (status.stopped)
        {
            printf(", STOPPED");
        }
    }
    if (status.request == PCF8563_FAILED)
    {
        printf(", FAILED: %s", i2c1_result_name(status.last_result));
    }
    if (verbose)
    {
        printf("; read %lu s ago, %lu reads, %lu writes, %lu failures", (unsigned long)age_s,
               (unsigned long)status.reads, (unsigned long)status.writes,
               (unsigned long)status.failures);
    }
    printf("\n");
}
