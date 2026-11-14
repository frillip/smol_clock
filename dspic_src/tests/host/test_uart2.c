// Host tests for drivers/uart2.c against a modelled UART peripheral.
#include <xc.h>
#include "../../drivers/uart2.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond)                                                  \
    do                                                               \
    {                                                                \
        checks++;                                                    \
        if (!(cond))                                                 \
        {                                                            \
            failures++;                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                            \
    } while (0)

void _U2RXInterrupt(void);
void _U2TXInterrupt(void);
void _U2ErrInterrupt(void);

// ---- Peripheral model ----------------------------------------------------
U2MODEBITS U2MODEbits;
U2STABITS U2STAbits;
IEC1BITS IEC1bits;
IEC4BITS IEC4bits;
IFS1BITS IFS1bits;
IFS4BITS IFS4bits;
uint16_t U2BRG;
static uint16_t scratch;
typedef struct
{
    uint8_t byte;
    uint8_t framing;
} RxEntry;
static RxEntry rx_fifo[4];
static int rx_count;
static uint16_t tx_fifo[4];
static int tx_count;
static uint8_t wire[4096];
static int wire_count;
static uint8_t rx_cleared_before_drain;

static void refresh(void)
{
    U2STAbits.URXDA = rx_count > 0;
    U2STAbits.FERR = rx_count > 0 && rx_fifo[0].framing;
    U2STAbits.UTXBF = tx_count >= 4;
    U2STAbits.TRMT = tx_count == 0;
}


uint16_t *fake_u2mode_reset(void)
{
    memset(&U2MODEbits, 0, sizeof U2MODEbits);
    return &scratch;
}


uint16_t *fake_u2sta_reset(void)
{
    memset(&U2STAbits, 0, sizeof U2STAbits);
    refresh();
    return &scratch;
}


uint16_t fake_u2rxreg_read(void)
{
    uint8_t b = rx_fifo[0].byte;
    memmove(rx_fifo, rx_fifo + 1, sizeof(RxEntry) * 3);
    rx_count--;
    refresh();
    return b;
}


// Each U2TXREG write lands directly in the next TX FIFO slot.
uint16_t *fake_u2txreg_slot(void)
{
    uint16_t *slot = &tx_fifo[tx_count++];
    refresh();
    return slot;
}


// Line activity: receive a byte (sets OERR if the FIFO is full).
static void line_receive(uint8_t byte, int framing)
{
    if (rx_count == 4)
    {
        U2STAbits.OERR = 1;
        return;
    }
    rx_fifo[rx_count].byte = byte;
    rx_fifo[rx_count].framing = (uint8_t)framing;
    rx_count++;
    refresh();
}


// Runs the TX interrupt while it is pending and enabled, shifting the FIFO
// onto the wire between interrupts (UTXISEL = 10: interrupt on FIFO empty).
static void run_tx_isr(void)
{
    while (IFS1bits.U2TXIF && IEC1bits.U2TXIE)
    {
        _U2TXInterrupt();
        for (int k = 0; k < tx_count; k++)
            wire[wire_count++] = (uint8_t)tx_fifo[k];
        if (tx_count > 0)
            IFS1bits.U2TXIF = 1;
        tx_count = 0;
        refresh();
    }
}


static void reset_model(void)
{
    rx_count = tx_count = wire_count = 0;
    memset(&IEC1bits, 0, sizeof IEC1bits);
    memset(&IEC4bits, 0, sizeof IEC4bits);
    memset(&IFS1bits, 0, sizeof IFS1bits);
    memset(&IFS4bits, 0, sizeof IFS4bits);
    refresh();
}


int main(void)
{
    uint8_t b;
    (void)rx_cleared_before_drain;

    printf("uart2\n");
    reset_model();
    CHECK(uart2_init(40000000UL, 115200UL) && U2BRG == 86);
    CHECK(uart2_init(40000000UL, 38400UL) && U2BRG == 259);
    CHECK(uart2_init(40000000UL, 9600UL) && U2BRG == 1041);
    CHECK(!uart2_init(40000000UL, 0UL));
    CHECK(!uart2_init(40000000UL, 100UL)); // divisor > 65536
    CHECK(uart2_init(40000000UL, 115200UL));
    CHECK(U2MODEbits.UARTEN && U2STAbits.UTXEN && IEC1bits.U2RXIE && IEC4bits.U2EIE);
    CHECK(U2STAbits.UTXISEL1 == 1 && U2STAbits.UTXISEL0 == 0);

    // Receive in order across several interrupts.
    for (int k = 0; k < 10; k++)
    {
        line_receive((uint8_t)k, 0);
        if (rx_count == 4)
            _U2RXInterrupt();
    }
    _U2RXInterrupt();
    CHECK(uart2_rx_available() == 10);
    int ok = 1;
    for (int k = 0; k < 10; k++)
        ok &= uart2_rx_pop(&b) && b == k;
    CHECK(ok && !uart2_rx_pop(&b));

    // Framing error: byte dropped, flag raised, recovery discards the ring.
    line_receive(0x55, 0);
    line_receive(0xAA, 1);
    line_receive(0x66, 0);
    _U2RXInterrupt();
    CHECK(uart2_rx_error_pending() && uart2_rx_available() == 2);
    CHECK(uart2_rx_recover() == UART2_RX_ERROR_FRAMING && uart2_rx_available() == 0);
    CHECK(uart2_rx_recover() == 0);

    // Overrun: the four FIFO bytes are drained before OERR is cleared.
    for (int k = 0; k < 5; k++)
        line_receive((uint8_t)(0x10 + k), 0);
    CHECK(U2STAbits.OERR);
    _U2ErrInterrupt();
    CHECK(IFS1bits.U2RXIF);
    _U2RXInterrupt();
    CHECK(!U2STAbits.OERR && uart2_rx_available() == 4);
    CHECK(uart2_rx_recover() == UART2_RX_ERROR_OVERRUN);

    // Software ring overflow: 511 usable bytes.
    for (int k = 0; k < 600; k++)
    {
        line_receive((uint8_t)k, 0);
        _U2RXInterrupt();
    }
    CHECK(uart2_rx_available() == UART2_RX_RING_SIZE - 1U);
    Uart2Stats stats;
    uart2_stats(&stats);
    CHECK(stats.rx_overflows == 600 - (UART2_RX_RING_SIZE - 1U));
    CHECK(uart2_rx_recover() & UART2_RX_ERROR_OVERFLOW);

    // Transmit: all-or-nothing, in order, interrupt disabled when empty.
    uint8_t message[300];
    for (int k = 0; k < 300; k++)
        message[k] = (uint8_t)(k * 3);
    CHECK(uart2_tx_write(message, 300));
    CHECK(IEC1bits.U2TXIE && IFS1bits.U2TXIF && uart2_tx_pending() == 300);
    CHECK(!uart2_tx_write(message, 300)); // 211 free: rejected whole
    CHECK(uart2_tx_pending() == 300);
    run_tx_isr();
    CHECK(wire_count == 300 && memcmp(wire, message, 300) == 0);
    CHECK(uart2_tx_pending() == 0 && !IEC1bits.U2TXIE && uart2_tx_done());

    // Masking restores exactly what was enabled.
    IEC1bits.T4IE = 1;
    IEC1bits.U2TXIE = 0;
    uart2_rx_discard();
    CHECK(IEC1bits.T4IE == 1 && IEC1bits.U2RXIE == 1 && IEC1bits.U2TXIE == 0 &&
          IEC4bits.U2EIE == 1);
    uart2_disable();
    CHECK(!uart2_tx_write(message, 1));

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
