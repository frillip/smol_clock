/*
 * Host test of uart1.c and console.c: formatted output through write(), the
 * primitive XC16's printf calls, to a model of the UART1 transmitter. (On
 * the host, glibc's printf does not route through a user write(), so the
 * test formats with snprintf and calls write() exactly as XC16's libc does.)
 *
 * Model: a 4-deep TX FIFO that sends one byte every BYTE_TIME status-register
 * reads (simulated time advances on every U1STA access), sets U1TXIF when the
 * FIFO empties (UTXISEL = 10), and runs the TX interrupt handler when it is
 * enabled and flagged, unless the test is holding interrupts off. Everything
 * sent is captured and compared with what was printed.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xc.h>
#include "../../drivers/uart1.h"
#include "../../console/console.h"

volatile IEC0BITS_t IEC0bits;
volatile IEC4BITS_t IEC4bits;
volatile IFS0BITS_t IFS0bits;
volatile IFS4BITS_t IFS4bits;
volatile IPC2BITS_t IPC2bits;
volatile IPC3BITS_t IPC3bits;
volatile IPC16BITS_t IPC16bits;
volatile U1MODEBITS_t U1MODEbits;
volatile uint16_t U1MODE, U1STA, U1BRG, U1RXREG;

void _U1TXInterrupt(void);
void _U1RXInterrupt(void);
int write(int handle, void *buffer, unsigned int len);

#define BYTE_TIME 3
static volatile U1STABITS_t status;
static uint16_t tx_slot;
static int fifo, clock_ticks;
static bool in_isr, interrupts_held;
static char sent[200000];
static size_t sent_len;
static long isr_runs;

static void advance(void)
{
    if (fifo > 0 && ++clock_ticks >= BYTE_TIME)
    {
        clock_ticks = 0;
        if (--fifo == 0 && status.UTXISEL1)
            IFS0bits.U1TXIF = 1; // FIFO empty
    }
    status.UTXBF = (fifo >= 4);
    status.TRMT = (fifo == 0);
    if (!in_isr && !interrupts_held && IEC0bits.U1TXIE && IFS0bits.U1TXIF)
    {
        in_isr = true;
        isr_runs++;
        _U1TXInterrupt();
        in_isr = false;
    }
}


volatile U1STABITS_t *fake_u1sta(void)
{
    advance();
    return &status;
}


// Called for each store to U1TXREG; the previous store is complete.
volatile uint16_t *fake_u1txreg(void)
{
    static bool pending;
    if (pending)
        sent[sent_len++] = (char)tx_slot;
    if (fifo >= 4)
    {
        printf("  FAIL: write to a full FIFO\n");
        exit(1);
    }
    fifo++;
    pending = true;
    return &tx_slot;
}


static void flush_capture(void)
{
    // Collect the last stored byte.
    fake_u1txreg();
    fifo--;
}


static int failures, checks;
#define CHECK(c)                                          \
    do                                                    \
    {                                                     \
        checks++;                                         \
        if (!(c))                                         \
        {                                                 \
            failures++;                                   \
            printf("  FAIL line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

int main(void)
{
    static char expected[200000];
    size_t expected_len = 0;
    Uart1Stats stats;
    int line;

    // Output before console_init() is dropped, not queued or crashed on.
    CHECK(write(1, "lost", 4) == 4);

    CHECK(console_init(40000000UL));
    CHECK(U1BRG == 86); // 40 MHz / (4 x 87) = 114943 baud
    CHECK(U1MODEbits.BRGH && U1MODEbits.UARTEN && status.UTXEN);
    CHECK(IPC2bits.U1RXIP == 1 && IPC3bits.U1TXIP == 1 && IPC16bits.U1EIP == 1);

    // 1. Normal running: status lines like main.c's, the TX interrupt
    // draining the ring as the FIFO empties.
    for (line = 0; line < 2000; line++)
    {
        char text[160];
        int n = snprintf(text, sizeof text,
                         "%05d GNSS: cap=%10lu IC12=%04X:%04X FIX:0 V:1 RAW:%luHz\r\n", line,
                         23754758UL + 40000000UL * (unsigned long)line, line & 0xFFFF,
                         (line * 7) & 0xFFFF, 39999924UL);
        memcpy(expected + expected_len, text, (size_t)n);
        expected_len += (size_t)n;
        CHECK(write(1, text, (unsigned int)n) == n);
        while (uart1_tx_pending() > 600)
            (void)U1STAbits; // time passes
    }

    CHECK(console_stalls() == 0); // the ring absorbed it all

    // 2. Interrupts held off (as inside __builtin_disi): a burst larger than
    // the ring must still complete, by write() feeding the UART itself.
    interrupts_held = true;
    for (line = 0; line < 40; line++)
    {
        char text[160];
        int n = snprintf(text, sizeof text,
                         "*** held-off burst line %02d: "
                         "the quick brown fox jumps over the lazy dog ***\r\n",
                         line);
        memcpy(expected + expected_len, text, (size_t)n);
        expected_len += (size_t)n;
        CHECK(write(2, text, (unsigned int)n) == n); // stderr goes there too
    }
    CHECK(console_stalls() > 0);
    interrupts_held = false;

    console_flush();
    flush_capture();
    CHECK(uart1_tx_done());
    CHECK(sent_len == expected_len);
    CHECK(memcmp(sent, expected, expected_len) == 0);
    uart1_stats(&stats);
    CHECK(stats.tx_bytes == expected_len);
    CHECK(stats.tx_rejections == 0);
    CHECK(stats.tx_high_water <= UART1_TX_RING_SIZE - 1U);
    CHECK(IEC0bits.U1TXIE == 0); // idle: interrupt switched off

    printf("console: %lu bytes through write() -> UART1, byte-exact; "
           "%lu TX interrupts; %lu stalls (all in the held-off burst); "
           "ring high water %u of %u\n",
           (unsigned long)sent_len, isr_runs, (unsigned long)console_stalls(), stats.tx_high_water,
           UART1_TX_RING_SIZE - 1U);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
