/*
 * I2C1 master. See i2c1.h for the model.
 *
 * The interrupt runs once per bus event the module finishes: a start, a
 * repeated start, a byte sent (with the slave's ACK or NACK latched in
 * ACKSTAT), a byte received, an ACK sent, a stop. Each time it issues the
 * next event and returns. The queue is a list of the callers' transfers:
 * head is the one on the bus (or the next to go), tail the last queued.
 *
 * Who writes what: the interrupt advances head and the state, and starts
 * the next transfer; the foreground adds at the tail and, only after a
 * fault or timeout, resets the module and moves head on, both with MI2C1
 * masked.
 */
#include "i2c1.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <xc.h>

#include "../platform/board.h"
#include "../platform/system.h"

// I2C1BRG = (FCY / FSCL - FCY x 130 ns) - 2 (the data sheet's formula; the
// 130 ns is the pulse gobbler delay): 93 at 40 MHz and 400 kHz, as V2.
#define I2C1_BRG ((FCY / I2C1_SCL_HZ) - (FCY / 7692308UL) - 2UL)
#if I2C1_BRG < 2UL || I2C1_BRG > 511UL
#error I2C1 baud rate out of range
#endif

// Bus recovery: SCL half-periods of 5 us (100 kHz), at most this many
// clocks: a slave part-way through a byte needs up to 8 and its ACK slot.
#define RECOVERY_HALF_PERIOD_US 5U
#define RECOVERY_CLOCKS_MAX     16U

typedef enum
{
    STATE_IDLE = 0,
    STATE_START,     // start condition in progress
    STATE_WRITE,     // address+W or a data byte in progress
    STATE_RESTART,   // repeated start in progress
    STATE_ADDRESS_R, // address+R in progress
    STATE_READ,      // receiving a byte
    STATE_ACK,       // sending ACK or NACK after a byte
    STATE_STOP,      // stop condition in progress
    STATE_FAULT      // collision: waiting for i2c1_service() to recover
} State;

static I2cTransfer *volatile head;
static I2cTransfer *volatile tail;
static volatile State state;
static volatile I2cResult pending; // the result to report once stopped
static volatile uint8_t position;  // bytes written or read so far
static volatile bool address_sent; // in STATE_WRITE: the address went
static volatile uint16_t started;  // counts transfers started, for timeouts
static volatile I2c1Stats stats;
static uint32_t (*stamp_clock)(void);

// The service's view, for timeouts.
static uint16_t watched;
static uint32_t watched_since_ms;
static bool watching;

// Only MI2C1 is masked. Constant assignments compile to single BSET/BCLR,
// so no other IEC1 bit (T4IE, CNIE) can be lost to a read-modify-write.
static bool mask_interrupt(void)
{
    bool enabled = IEC1bits.MI2C1IE != 0;

    IEC1bits.MI2C1IE = 0;
    return enabled;
}


static void restore_interrupt(bool enabled)
{
    if (enabled)
    {
        IEC1bits.MI2C1IE = 1;
    }
}


static void delay_half_period(void)
{
    __delay_us(RECOVERY_HALF_PERIOD_US);
}


/*
 * With the module off, the pins are ports: LAT 0, and TRIS decides between
 * pulling low (output) and letting the pull-up have it (input), so the
 * lines are only ever driven low, as open drain. Clocks until SDA is high,
 * then makes a stop (SDA rising while SCL is high), which every slave takes
 * as the end of whatever it was doing.
 */
static void recover_bus(void)
{
    uint16_t clocks = 0;

    I2C1CONbits.I2CEN = 0;
    I2C1_SCL_RELEASE();
    I2C1_SDA_RELEASE();
    delay_half_period();
    while (!I2C1_SDA_IS_HIGH() && clocks < RECOVERY_CLOCKS_MAX)
    {
        I2C1_SCL_PULL_LOW();
        delay_half_period();
        I2C1_SCL_RELEASE();
        delay_half_period();
        clocks++;
    }
    // The stop: SDA low while SCL is low, then SCL up, then SDA up.
    I2C1_SCL_PULL_LOW();
    delay_half_period();
    I2C1_SDA_PULL_LOW();
    delay_half_period();
    I2C1_SCL_RELEASE();
    delay_half_period();
    I2C1_SDA_RELEASE();
    delay_half_period();

    stats.recoveries++;
    stats.recovery_clocks = clocks;
    if (!I2C1_SDA_IS_HIGH() || !I2C1_SCL_IS_HIGH())
    {
        stats.stuck++;
    }
}


static void enable_module(void)
{
    I2C1BRG = (uint16_t)I2C1_BRG;
    I2C1STAT = 0;
    // I2CEN; 7-bit addresses; DISSLW clear: slew rate control on, which is
    // meant for 400 kHz. As V2.
    I2C1CON = 0x8000;
}


// From the interrupt or with it masked: the next transfer, if any, starts.
static void start_next(void)
{
    if (head == 0)
    {
        state = STATE_IDLE;
        return;
    }
    head->result = I2C_BUSY;
    position = 0;
    address_sent = false;
    pending = I2C_OK;
    started++;
    state = STATE_START;
    I2C1CONbits.SEN = 1;
}


// The transfer at the head is over: it leaves the queue with its result.
static void finish(I2cResult result)
{
    I2cTransfer *done = head;

    head = done->next;
    if (head == 0)
    {
        tail = 0;
    }
    done->next = 0;
    stats.transfers++;
    stats.results[result]++;
    done->result = result; // last: the caller may reuse it from here
}


static void stop(I2cResult result)
{
    pending = result;
    state = STATE_STOP;
    I2C1CONbits.PEN = 1;
}


// After a byte written (or the address+W) was acknowledged.
static void write_next(I2cTransfer *transfer)
{
    if (position < transfer->write_length)
    {
        state = STATE_WRITE;
        I2C1TRN = transfer->write[position++];
    }
    else if (transfer->read_length != 0U)
    {
        state = STATE_RESTART;
        I2C1CONbits.RSEN = 1;
    }
    else
    {
        stop(I2C_OK);
    }
}


void i2c1_init(void)
{
    IEC1bits.MI2C1IE = 0;
    head = 0;
    tail = 0;
    state = STATE_IDLE;
    watching = false;
    recover_bus();
    enable_module();
    IFS1bits.MI2C1IF = 0;
    IPC4bits.MI2C1IP = I2C1_INTERRUPT_PRIORITY;
    IEC1bits.MI2C1IE = 1;
}


bool i2c1_submit(I2cTransfer *transfer)
{
    bool enabled;

    if (transfer == 0 || transfer->address > 0x7FU ||
        (transfer->write_length != 0U && transfer->write == 0) ||
        (transfer->read_length != 0U && transfer->read == 0))
    {
        return false;
    }
    enabled = mask_interrupt();
    if (transfer->result == I2C_QUEUED || transfer->result == I2C_BUSY)
    {
        restore_interrupt(enabled);
        return false;
    }
    transfer->result = I2C_QUEUED;
    transfer->next = 0;
    if (tail != 0)
    {
        tail->next = transfer;
    }
    else
    {
        head = transfer;
    }
    tail = transfer;
    if (state == STATE_IDLE)
    {
        start_next();
    }
    restore_interrupt(enabled);
    return true;
}


bool i2c1_done(const I2cTransfer *transfer)
{
    I2cResult result = transfer->result;

    return result != I2C_QUEUED && result != I2C_BUSY;
}


bool i2c1_idle(void)
{
    return head == 0;
}


void i2c1_service(uint32_t now_ms)
{
    bool enabled;
    bool fault;
    bool expired;

    enabled = mask_interrupt();
    fault = state == STATE_FAULT;
    if (state == STATE_IDLE || fault)
    {
        watching = false;
    }
    else if (!watching || watched != started)
    {
        watching = true;
        watched = started;
        watched_since_ms = now_ms;
    }
    expired = watching && now_ms - watched_since_ms > I2C1_TIMEOUT_MS;
    if (expired || fault)
    {
        // A timed-out transfer leaves now; a faulted one already has.
        if (expired && head != 0)
        {
            finish(I2C_TIMEOUT);
        }
        watching = false;
        recover_bus();
        enable_module();
        IFS1bits.MI2C1IF = 0;
        start_next();
    }
    restore_interrupt(enabled);
}


void i2c1_set_stamp_clock(uint32_t (*clock)(void))
{
    bool enabled = mask_interrupt();

    stamp_clock = clock;
    restore_interrupt(enabled);
}


const I2c1Stats *i2c1_stats(void)
{
    return (const I2c1Stats *)&stats;
}


const char *i2c1_result_name(I2cResult result)
{
    static const char *const names[I2C_RESULT_COUNT] = {
        "idle", "queued", "busy", "ok", "no answer", "refused", "bus error", "timeout"};

    return result < I2C_RESULT_COUNT ? names[result] : "?";
}


void i2c1_print(void)
{
    const I2c1Stats *s = i2c1_stats();

    printf("I2C1: %lu transfers, %lu no answer, %lu refused, %lu bus errors, %lu timeouts, "
           "%lu recoveries (%u clocks last",
           (unsigned long)s->transfers, (unsigned long)s->results[I2C_NACK_ADDRESS],
           (unsigned long)s->results[I2C_NACK_DATA], (unsigned long)s->results[I2C_BUS_ERROR],
           (unsigned long)s->results[I2C_TIMEOUT], (unsigned long)s->recoveries,
           s->recovery_clocks);
    printf("%s)\n", s->stuck != 0U ? ", BUS STUCK" : "");
}


// --------------------------------------------------------------------------
// Interrupt handler
// --------------------------------------------------------------------------

void __attribute__((interrupt, no_auto_psv)) _MI2C1Interrupt(void)
{
    I2cTransfer *transfer = head;

    IFS1bits.MI2C1IF = 0;
    if (I2C1STATbits.BCL || I2C1STATbits.IWCOL)
    {
        // Lost the bus or wrote at the wrong moment: the module cannot be
        // trusted to make a stop. The service recovers the bus.
        I2C1STATbits.BCL = 0;
        I2C1STATbits.IWCOL = 0;
        if (transfer != 0 && state != STATE_IDLE && state != STATE_FAULT)
        {
            finish(I2C_BUS_ERROR);
        }
        state = STATE_FAULT;
        return;
    }
    if (transfer == 0)
    {
        state = STATE_IDLE;
        return;
    }
    switch (state)
    {
        case STATE_START:
            if (stamp_clock != 0)
            {
                transfer->start_stamp = stamp_clock();
            }
            if (transfer->write_length != 0U || transfer->read_length == 0U)
            {
                state = STATE_WRITE;
                I2C1TRN = (uint8_t)(transfer->address << 1);
            }
            else
            {
                state = STATE_ADDRESS_R;
                I2C1TRN = (uint8_t)((transfer->address << 1) | 1U);
            }
            break;
        case STATE_WRITE:
            if (I2C1STATbits.ACKSTAT)
            {
                stop(address_sent ? I2C_NACK_DATA : I2C_NACK_ADDRESS);
                break;
            }
            address_sent = true;
            write_next(transfer);
            break;
        case STATE_RESTART:
            state = STATE_ADDRESS_R;
            I2C1TRN = (uint8_t)((transfer->address << 1) | 1U);
            break;
        case STATE_ADDRESS_R:
            if (I2C1STATbits.ACKSTAT)
            {
                // Refused only here, after a write: the address answered.
                stop(transfer->write_length != 0U ? I2C_NACK_DATA : I2C_NACK_ADDRESS);
                break;
            }
            position = 0;
            state = STATE_READ;
            I2C1CONbits.RCEN = 1;
            break;
        case STATE_READ:
            transfer->read[position++] = (uint8_t)I2C1RCV;
            // ACK every byte but the last, which is NACKed to end the read.
            I2C1CONbits.ACKDT = position < transfer->read_length ? 0 : 1;
            state = STATE_ACK;
            I2C1CONbits.ACKEN = 1;
            break;
        case STATE_ACK:
            if (position < transfer->read_length)
            {
                state = STATE_READ;
                I2C1CONbits.RCEN = 1;
            }
            else
            {
                stop(I2C_OK);
            }
            break;
        case STATE_STOP:
            finish(pending);
            start_next();
            break;
        default: break; // IDLE, FAULT: nothing of ours
    }
}
