#include "host_uart2.h"
#include "../../drivers/uart2.h"
#include <string.h>

#define QUEUE_SIZE 8192U
static uint8_t rx_queue[QUEUE_SIZE];
static uint16_t rx_head, rx_tail;
static uint8_t tx_queue[QUEUE_SIZE];
static uint16_t tx_head, tx_tail;
static uint16_t error_flags;
static uint32_t baud;
static uint32_t init_count;
static Uart2Stats stats;

static uint16_t count(uint16_t head, uint16_t tail)
{
    return (uint16_t)((head - tail) % QUEUE_SIZE);
}


void host_uart2_reset(void)
{
    rx_head = rx_tail = tx_head = tx_tail = 0;
    error_flags = 0;
    baud = 0;
    init_count = 0;
    memset(&stats, 0, sizeof(stats));
}


uint32_t host_uart2_baud(void)
{
    return baud;
}


uint32_t host_uart2_init_count(void)
{
    return init_count;
}


void host_uart2_inject_rx(const uint8_t *bytes, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++)
    {
        uint16_t next = (uint16_t)((rx_head + 1U) % QUEUE_SIZE);
        if (next == rx_tail)
        {
            error_flags |= UART2_RX_ERROR_OVERFLOW;
            return;
        }
        rx_queue[rx_head] = bytes[i];
        rx_head = next;
    }
    stats.rx_bytes += length;
}


void host_uart2_inject_error(uint16_t flags)
{
    error_flags |= flags;
}


uint16_t host_uart2_take_tx(uint8_t *destination, uint16_t capacity)
{
    uint16_t taken = 0;
    while (taken < capacity && tx_tail != tx_head)
    {
        destination[taken++] = tx_queue[tx_tail];
        tx_tail = (uint16_t)((tx_tail + 1U) % QUEUE_SIZE);
    }
    return taken;
}


bool uart2_init(uint32_t instruction_hz, uint32_t baud_rate)
{
    (void)instruction_hz;
    baud = baud_rate;
    init_count++;
    rx_head = rx_tail = tx_head = tx_tail = 0;
    error_flags = 0;
    return baud_rate != 0U;
}


void uart2_enable(void)
{
}


void uart2_disable(void)
{
}


uint16_t uart2_rx_available(void)
{
    return count(rx_head, rx_tail);
}


bool uart2_rx_pop(uint8_t *byte)
{
    if (rx_tail == rx_head)
        return false;
    *byte = rx_queue[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) % QUEUE_SIZE);
    return true;
}


uint16_t uart2_rx_read(uint8_t *destination, uint16_t limit)
{
    uint16_t n = 0;
    while (n < limit && uart2_rx_pop(&destination[n]))
        n++;
    return n;
}


void uart2_rx_discard(void)
{
    rx_tail = rx_head;
}


bool uart2_rx_error_pending(void)
{
    return error_flags != 0U;
}


uint16_t uart2_rx_recover(void)
{
    uint16_t e = error_flags;
    if (e)
    {
        rx_tail = rx_head;
        error_flags = 0;
    }
    return e;
}


uint16_t uart2_tx_pending(void)
{
    return count(tx_head, tx_tail);
}


uint16_t uart2_tx_free(void)
{
    return (uint16_t)(511U - (uart2_tx_pending() > 511U ? 511U : uart2_tx_pending()));
}


bool uart2_tx_write(const uint8_t *source, uint16_t length)
{
    if (length > uart2_tx_free())
    {
        stats.tx_rejections++;
        return false;
    }
    for (uint16_t i = 0; i < length; i++)
    {
        tx_queue[tx_head] = source[i];
        tx_head = (uint16_t)((tx_head + 1U) % QUEUE_SIZE);
    }
    stats.tx_bytes += length;
    return true;
}


bool uart2_tx_done(void)
{
    return tx_head == tx_tail;
}


void uart2_stats(Uart2Stats *destination)
{
    *destination = stats;
}


void uart2_stats_clear(void)
{
    memset(&stats, 0, sizeof(stats));
}
