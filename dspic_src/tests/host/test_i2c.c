/*
 * The I2C1 master (drivers/i2c1.c) and the PCF8563 driver (drivers/pcf8563.c)
 * against a model of the I2C1 module and the bus.
 *
 * The model completes whatever the driver started (a start, a byte written
 * to I2C1TRN, a repeated start, a receive, an ACK, a stop) and raises the
 * master interrupt, which runs whenever it is enabled, until the bus has
 * nothing more to do. Slaves on the bus: an emulated PCF8563 at 0x51 and a
 * device at 0x23 that refuses its second data byte. Faults: a collision at
 * a chosen event, a module that stops responding, SDA held low for a number
 * of recovery clocks.
 */
#include <stdio.h>
#include <string.h>

#include <xc.h>

#include "../../drivers/i2c1.h"
#include "../../drivers/pcf8563.h"
#include "../../platform/utc.h"

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

volatile I2C1CONBITS I2C1CONbits;
volatile I2C1STATBITS I2C1STATbits;
volatile IFS1BITS IFS1bits;
volatile IEC1BITS IEC1bits;
volatile IPC4BITS IPC4bits;
volatile uint16_t I2C1BRG;
volatile uint16_t I2C1RCV;
volatile unsigned _LATB8, _LATB9, _TRISB8, _TRISB9;

void _MI2C1Interrupt(void);

// ---- The bus model ---------------------------------------------------------
static volatile uint16_t trn;
static int trn_written;
static int addressed;   // -1: no slave selected
static int reading;     // the selected slave is being read
static int first_write; // the next byte written is the slave's first
static int collide_at;  // raise a collision at this event (0: never)
static int events;
static int dead;          // the module stops completing anything
static int sda_low_reads; // recovery: SDA reads low this many more times
static int stops, starts;
static int acks_sent, nacks_sent;

volatile uint16_t *fake_i2c_trn(void)
{
    trn_written = 1;
    return &trn;
}


int fake_scl(void)
{
    return 1;
}


int fake_sda(void)
{
    if (sda_low_reads > 0)
    {
        sda_low_reads--;
        return 0;
    }
    return 1;
}


// The PCF8563: sixteen registers and an address pointer.
static uint8_t rtc[16];
static uint8_t rtc_pointer;
static int rtc_present = 1;

static int slave_address(uint8_t address)
{
    return (address == PCF8563_ADDRESS && rtc_present) || address == 0x23U;
}


// A byte written to the selected slave; true for ACK.
static int slave_write(uint8_t byte)
{
    static int count;

    if (addressed == 0x23)
    {
        count = first_write ? 1 : count + 1;
        first_write = 0;
        return count < 2;
    }
    if (first_write)
    {
        rtc_pointer = byte & 0x0FU;
    }
    else
    {
        rtc[rtc_pointer] = byte;
        rtc_pointer = (uint8_t)((rtc_pointer + 1U) & 0x0FU);
    }
    first_write = 0;
    return 1;
}


static uint8_t slave_read(void)
{
    uint8_t byte;

    if (addressed != PCF8563_ADDRESS)
    {
        return 0xFFU;
    }
    byte = rtc[rtc_pointer];
    rtc_pointer = (uint8_t)((rtc_pointer + 1U) & 0x0FU);
    return byte;
}


// Completes one pending action; true if there was one.
static int complete_one(void)
{
    if (dead || !I2C1CONbits.I2CEN)
    {
        return 0;
    }
    if (I2C1CONbits.SEN || I2C1CONbits.RSEN || I2C1CONbits.PEN || I2C1CONbits.RCEN ||
        I2C1CONbits.ACKEN || trn_written)
    {
        events++;
        if (collide_at != 0 && events == collide_at)
        {
            I2C1CONbits.SEN = I2C1CONbits.RSEN = I2C1CONbits.PEN = 0;
            I2C1CONbits.RCEN = I2C1CONbits.ACKEN = 0;
            trn_written = 0;
            I2C1STATbits.BCL = 1;
            IFS1bits.MI2C1IF = 1;
            return 1;
        }
    }
    if (I2C1CONbits.SEN || I2C1CONbits.RSEN)
    {
        I2C1CONbits.SEN = 0;
        I2C1CONbits.RSEN = 0;
        addressed = -1;
        starts++;
    }
    else if (trn_written)
    {
        trn_written = 0;
        if (addressed < 0)
        {
            uint8_t address = (uint8_t)(trn >> 1);

            if (slave_address(address))
            {
                addressed = address;
                reading = trn & 1U;
                first_write = 1;
                I2C1STATbits.ACKSTAT = 0;
            }
            else
            {
                I2C1STATbits.ACKSTAT = 1;
            }
        }
        else
        {
            I2C1STATbits.ACKSTAT = slave_write((uint8_t)trn) ? 0 : 1;
        }
    }
    else if (I2C1CONbits.RCEN)
    {
        I2C1CONbits.RCEN = 0;
        I2C1RCV = slave_read();
    }
    else if (I2C1CONbits.ACKEN)
    {
        I2C1CONbits.ACKEN = 0;
        if (I2C1CONbits.ACKDT)
        {
            nacks_sent++;
        }
        else
        {
            acks_sent++;
        }
    }
    else if (I2C1CONbits.PEN)
    {
        I2C1CONbits.PEN = 0;
        addressed = -1;
        stops++;
    }
    else
    {
        return 0;
    }
    IFS1bits.MI2C1IF = 1;
    return 1;
}


// Runs the bus until it is quiet: each completed action interrupts.
static void run_bus(void)
{
    int guard = 0;

    while (guard++ < 10000)
    {
        if (IFS1bits.MI2C1IF && IEC1bits.MI2C1IE)
        {
            _MI2C1Interrupt();
            continue;
        }
        if (!complete_one())
        {
            break;
        }
    }
}


static void reset_model(void)
{
    trn_written = 0;
    addressed = -1;
    collide_at = 0;
    events = 0;
    dead = 0;
    sda_low_reads = 0;
    memset((void *)&I2C1CONbits, 0, sizeof(I2C1CONbits));
    memset((void *)&I2C1STATbits, 0, sizeof(I2C1STATbits));
    IFS1bits.MI2C1IF = 0;
}


// ---- Helpers --------------------------------------------------------------
static void set_rtc_time(uint32_t utc, int vl, int stop, int century_override)
{
    UtcCivil c;

    utc_to_civil(utc, false, &c);
    rtc[0] = stop ? PCF8563_STOP : 0U;
    rtc[1] = 0;
    rtc[2] = (uint8_t)(pcf8563_binary_to_bcd(c.second) | (vl ? PCF8563_VL : 0U));
    rtc[3] = pcf8563_binary_to_bcd(c.minute);
    rtc[4] = pcf8563_binary_to_bcd(c.hour);
    rtc[5] = pcf8563_binary_to_bcd(c.day);
    rtc[6] = 0;
    rtc[7] = (uint8_t)(pcf8563_binary_to_bcd(c.month) |
                       ((century_override >= 0 ? century_override : c.year >= 2100U) ? 0x80U : 0U));
    rtc[8] = pcf8563_binary_to_bcd((uint8_t)(c.year % 100U));
}


static uint32_t civil(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s)
{
    UtcCivil c = {y, mo, d, h, mi, s};
    return utc_from_civil(&c);
}


// ---- I2C tests ----------------------------------------------------------
static void test_init(void)
{
    reset_model();
    sda_low_reads = 3; // a slave left part-way through a byte
    i2c1_init();
    CHECK(I2C1CONbits.I2CEN == 1);
    CHECK(I2C1BRG == 93U);
    CHECK(IEC1bits.MI2C1IE == 1);
    CHECK(IPC4bits.MI2C1IP == I2C1_INTERRUPT_PRIORITY);
    CHECK(i2c1_stats()->recoveries == 1U);
    CHECK(i2c1_stats()->recovery_clocks == 3U); // a clock per low read
    CHECK(i2c1_stats()->stuck == 0U);
    CHECK(_TRISB8 == 1 && _TRISB9 == 1); // released to the module
}


static void test_probe_and_read(void)
{
    static uint8_t reg = 0x02;
    static uint8_t data[7];
    static I2cTransfer probe_absent = I2C_TRANSFER(0x20, 0, 0, 0, 0);
    static I2cTransfer probe_rtc = I2C_TRANSFER(PCF8563_ADDRESS, 0, 0, 0, 0);
    static I2cTransfer read = I2C_TRANSFER(PCF8563_ADDRESS, &reg, 1, data, 7);
    int ack_before = acks_sent, nack_before = nacks_sent, stops_before = stops;

    reset_model();
    I2C1CONbits.I2CEN = 1;
    for (int i = 0; i < 16; i++)
    {
        rtc[i] = (uint8_t)(0x10 + i);
    }
    CHECK(i2c1_done(&probe_absent)); // never submitted
    CHECK(i2c1_submit(&probe_absent));
    CHECK(!i2c1_done(&probe_absent));
    CHECK(!i2c1_submit(&probe_absent)); // already queued
    CHECK(i2c1_submit(&probe_rtc));
    CHECK(i2c1_submit(&read));
    run_bus();
    CHECK(probe_absent.result == I2C_NACK_ADDRESS);
    CHECK(probe_rtc.result == I2C_OK);
    CHECK(read.result == I2C_OK);
    for (int i = 0; i < 7; i++)
    {
        CHECK(data[i] == 0x12 + i);
    }
    CHECK(acks_sent - ack_before == 6); // every byte but the last
    CHECK(nacks_sent - nack_before == 1);
    CHECK(stops - stops_before == 3); // each ends with a stop, a NACK too
    CHECK(i2c1_submit(&read));        // done: can go again
    run_bus();
    CHECK(read.result == I2C_OK);
}


static void test_refused_byte(void)
{
    static const uint8_t bytes[3] = {1, 2, 3};
    static I2cTransfer write = I2C_TRANSFER(0x23, bytes, 3, 0, 0);
    int stops_before = stops;

    reset_model();
    I2C1CONbits.I2CEN = 1;
    CHECK(i2c1_submit(&write));
    run_bus();
    CHECK(write.result == I2C_NACK_DATA);
    CHECK(stops - stops_before == 1);
}


static void test_bad_submissions(void)
{
    static uint8_t buffer[2];
    I2cTransfer bad_address = I2C_TRANSFER(0x80, 0, 0, 0, 0);
    I2cTransfer no_write_buffer = I2C_TRANSFER(0x51, 0, 2, 0, 0);
    I2cTransfer no_read_buffer = I2C_TRANSFER(0x51, buffer, 1, 0, 2);

    CHECK(!i2c1_submit(&bad_address));
    CHECK(!i2c1_submit(&no_write_buffer));
    CHECK(!i2c1_submit(&no_read_buffer));
    CHECK(!i2c1_submit(0));
}


static void test_collision(void)
{
    static uint8_t reg = 0x02;
    static uint8_t data[3];
    static I2cTransfer first = I2C_TRANSFER(PCF8563_ADDRESS, &reg, 1, data, 3);
    static I2cTransfer second = I2C_TRANSFER(PCF8563_ADDRESS, &reg, 1, data, 3);
    uint32_t recoveries = i2c1_stats()->recoveries;

    reset_model();
    I2C1CONbits.I2CEN = 1;
    collide_at = 3; // start, address, then the register byte collides
    CHECK(i2c1_submit(&first));
    CHECK(i2c1_submit(&second));
    run_bus();
    CHECK(first.result == I2C_BUS_ERROR);
    CHECK(second.result == I2C_QUEUED); // waits for the recovery
    i2c1_service(1000);
    CHECK(i2c1_stats()->recoveries == recoveries + 1U);
    CHECK(I2C1CONbits.I2CEN == 1);
    run_bus();
    CHECK(second.result == I2C_OK);
    CHECK(I2C1STATbits.BCL == 0);
}


static void test_timeout(void)
{
    static I2cTransfer stuck = I2C_TRANSFER(PCF8563_ADDRESS, 0, 0, 0, 0);
    static I2cTransfer next = I2C_TRANSFER(PCF8563_ADDRESS, 0, 0, 0, 0);
    uint32_t recoveries = i2c1_stats()->recoveries;

    reset_model();
    I2C1CONbits.I2CEN = 1;
    dead = 1; // the start never completes
    CHECK(i2c1_submit(&stuck));
    CHECK(i2c1_submit(&next));
    run_bus();
    i2c1_service(5000);
    CHECK(stuck.result == I2C_BUSY);
    i2c1_service(5000 + I2C1_TIMEOUT_MS);
    CHECK(stuck.result == I2C_BUSY); // not yet over the limit
    dead = 0;
    i2c1_service(5000 + I2C1_TIMEOUT_MS + 1U);
    CHECK(stuck.result == I2C_TIMEOUT);
    CHECK(i2c1_stats()->recoveries == recoveries + 1U);
    run_bus();
    CHECK(next.result == I2C_OK);
    // A healthy bus never times out, however rarely the service runs.
    i2c1_service(100000);
    i2c1_service(200000);
    CHECK(next.result == I2C_OK);
}


static void test_stuck_bus(void)
{
    uint32_t stuck = i2c1_stats()->stuck;

    reset_model();
    sda_low_reads = 1000; // nothing lets go
    i2c1_init();
    CHECK(i2c1_stats()->recovery_clocks == 16U);
    CHECK(i2c1_stats()->stuck == stuck + 1U);
    sda_low_reads = 0;
}


// ---- PCF8563 tests ---------------------------------------------------------
static uint32_t now;
static Pcf8563Reference reference;

static void service_for(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10U)
    {
        now += 10U;
        run_bus();
        i2c1_service(now);
        (void)pcf8563_service(now);
    }
}


static void test_bcd(void)
{
    int ok = 1;

    for (uint8_t v = 0; v < 100U; v++)
    {
        uint8_t bcd = pcf8563_binary_to_bcd(v);

        ok =
            ok && bcd == (uint8_t)(((v / 10U) << 4) | (v % 10U)) && pcf8563_bcd_to_binary(bcd) == v;
    }
    CHECK(ok);
}


static void test_rtc_read(void)
{
    const Pcf8563Status *s = pcf8563_status();
    uint32_t t = civil(2026, 10, 21, 17, 30, 5);

    reset_model();
    I2C1CONbits.I2CEN = 1;
    rtc_present = 1;
    set_rtc_time(t, 0, 0, -1);
    pcf8563_init(now);
    service_for(20);
    CHECK(s->present);
    CHECK(s->calendar_valid);
    CHECK(s->time_valid);
    CHECK(s->utc_seconds == t);
    CHECK(s->reads == 1U);
    CHECK(s->request == PCF8563_DONE);
    CHECK(pcf8563_report_due() && !pcf8563_report_due()); // the first read, once

    // VL: readable, not usable.
    set_rtc_time(t, 1, 0, -1);
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(s->calendar_valid && !s->time_valid && s->voltage_low);
    CHECK(!pcf8563_report_due()); // a plain read is not reported
    // STOP likewise.
    set_rtc_time(t, 0, 1, -1);
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(s->calendar_valid && !s->time_valid && s->stopped);
    // Rubbish BCD: no date at all.
    set_rtc_time(t, 0, 0, -1);
    rtc[3] = 0x7A;
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(!s->calendar_valid && !s->time_valid);
    // 31 February.
    set_rtc_time(t, 0, 0, -1);
    rtc[5] = 0x31;
    rtc[7] = 0x02;
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(!s->calendar_valid);
    // The century bit: 2006 read as 2106.
    set_rtc_time(civil(2006, 1, 1, 0, 0, 0), 0, 0, 1);
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(s->calendar_valid && s->utc_seconds == civil(2106, 1, 1, 0, 0, 0));
}


static void test_rtc_write(void)
{
    const Pcf8563Status *s = pcf8563_status();
    uint32_t t = civil(2026, 10, 21, 17, 30, 5); // a Wednesday
    uint32_t late = civil(2100, 3, 1, 12, 0, 0); // a Monday
    uint32_t writes = s->writes;

    set_rtc_time(civil(2001, 1, 1, 0, 0, 0), 1, 1, -1); // power lost, stopped
    rtc[1] = 0x1F;
    CHECK(!pcf8563_write(PCF8563_FIRST_UTC - 1U)); // before 2000
    CHECK(pcf8563_write(t));
    CHECK(pcf8563_busy());
    CHECK(!pcf8563_read()); // one request at a time
    CHECK(!pcf8563_write(t));
    service_for(30);
    CHECK(s->writes == writes + 1U);
    CHECK(rtc[0] == 0x00 && rtc[1] == 0x00); // STOP clear, interrupts off
    CHECK(rtc[2] == 0x05 && rtc[3] == 0x30 && rtc[4] == 0x17 && rtc[5] == 0x21);
    CHECK(rtc[6] == 3); // Wednesday
    CHECK(rtc[7] == 0x10 && rtc[8] == 0x26);
    CHECK(s->time_valid && s->utc_seconds == t); // read back
    CHECK(pcf8563_report_due() && !pcf8563_report_due());
    CHECK(pcf8563_write(late));
    service_for(30);
    CHECK(rtc[7] == (0x80 | 0x03) && rtc[8] == 0x00 && rtc[6] == 1);
    CHECK(s->time_valid && s->utc_seconds == late);
}


// The RTC's time now, from its last read and the time since.
static void test_rtc_now(void)
{
    const Pcf8563Status *s = pcf8563_status();
    uint32_t t = civil(2026, 10, 21, 17, 33, 35);
    uint32_t read_ms;
    uint32_t rtc_now;

    set_rtc_time(t, 0, 0, -1);
    CHECK(pcf8563_read());
    service_for(10);
    read_ms = s->read_ms;
    CHECK(pcf8563_now(read_ms, &rtc_now) && rtc_now == t);
    CHECK(pcf8563_now(read_ms + 499U, &rtc_now) && rtc_now == t);
    CHECK(pcf8563_now(read_ms + 500U, &rtc_now) && rtc_now == t + 1U); // rounded
    CHECK(pcf8563_now(read_ms + 6000U, &rtc_now) && rtc_now == t + 6U);
    CHECK(pcf8563_now(read_ms + 86400000UL, &rtc_now) && rtc_now == t + 86400U);
    // The line, by eye: 6 s on, the clock 55 s behind, then unknown.
    reference.utc_seconds = t + 6U - 55U;
    reference.known = true;
    pcf8563_print(read_ms + 6000U, reference, false);
    pcf8563_print(read_ms + 6000U, reference, true);
    reference.known = false;
    pcf8563_print(read_ms + 6000U, reference, false);
    // No date: no time now.
    rtc[3] = 0x7A;
    CHECK(pcf8563_read());
    service_for(10);
    CHECK(!pcf8563_now(now, &rtc_now));
    pcf8563_print(now, reference, false);
    pcf8563_print(now, reference, true);
}


static void test_rtc_absent(void)
{
    const Pcf8563Status *s = pcf8563_status();
    uint32_t reads;

    rtc_present = 0;
    CHECK(pcf8563_read());
    service_for(20);
    CHECK(!s->present && !s->time_valid);
    CHECK(s->request == PCF8563_FAILED && s->last_result == I2C_NACK_ADDRESS);
    rtc_present = 1;
    set_rtc_time(civil(2026, 10, 21, 18, 0, 0), 0, 0, -1);
    reads = s->reads;
    service_for(PCF8563_DETECT_INTERVAL_MS - 100U);
    CHECK(!s->present); // not retried yet
    service_for(200U);
    CHECK(s->present && s->reads == reads + 1U && s->time_valid);
    // Present: read once a minute.
    service_for(PCF8563_READ_INTERVAL_MS + 20U);
    CHECK(s->reads == reads + 2U);
}


int main(void)
{
    printf("test_i2c\n");
    test_init();
    test_probe_and_read();
    test_refused_byte();
    test_bad_submissions();
    test_collision();
    test_timeout();
    test_stuck_bus();
    test_bcd();
    test_rtc_read();
    test_rtc_write();
    test_rtc_now();
    test_rtc_absent();
    i2c1_print();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
