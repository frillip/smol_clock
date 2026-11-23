#include <xc.h>
#include "console.h"
#include "../drivers/uart1.h"

#include <stddef.h>
#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

static uint32_t stalls;
static uint32_t written_bytes;
static uint32_t write_calls;
static ConsoleSink sink;

bool console_init(uint32_t instruction_hz)
{
    IPC2bits.U1RXIP = CONSOLE_INTERRUPT_PRIORITY;
    IPC3bits.U1TXIP = CONSOLE_INTERRUPT_PRIORITY;
    IPC16bits.U1EIP = CONSOLE_INTERRUPT_PRIORITY;
    return uart1_init(instruction_hz, CONSOLE_BAUD_RATE);
}


void console_push(void)
{
    format_flush();
}


uint32_t console_stalls(void)
{
    return stalls;
}


void console_counts(uint32_t *bytes, uint32_t *calls)
{
    *bytes = written_bytes;
    *calls = write_calls;
}


void console_set_sink(ConsoleSink new_sink)
{
    console_push(); // what was printed before goes where it was meant to
    sink = new_sink;
}


int write(int handle, void *buffer, unsigned int len);

void console_write(const char *text, uint16_t length)
{
    (void)write(1, (void *)text, length);
}


void console_flush(void)
{
    console_push();
    while (uart1_is_enabled() && !uart1_tx_done())
    {
        uart1_tx_poll();
    }
}


/*
 * The libc output primitive. handle is 1 (stdout) or 2 (stderr); both go to
 * the console. Returns len: every byte is either queued or, before
 * console_init(), deliberately dropped, so the library never sees a short
 * write it would have to retry.
 */
int __attribute__((__section__(".libc.write"))) write(int handle, void *buffer, unsigned int len)
{
    const uint8_t *next = (const uint8_t *)buffer;
    unsigned int remaining = len;
    bool stalled = false;

    (void)handle;
    written_bytes += len;
    write_calls++;
    if (!uart1_is_enabled() || buffer == NULL)
    {
        return (int)len;
    }
    if (sink != NULL)
    {
        while (remaining != 0U)
        {
            uint16_t chunk = remaining > 0x4000U ? 0x4000U : (uint16_t)remaining;
            sink(next, chunk);
            next += chunk;
            remaining -= chunk;
        }
        return (int)len;
    }
    while (remaining != 0U)
    {
        uint16_t queued = uart1_tx_write_some(next, (uint16_t)remaining);

        next += queued;
        remaining -= queued;
        if (remaining != 0U)
        {
            // Ring full: push bytes into the UART ourselves, which also
            // works while its interrupt cannot run.
            if (!stalled)
            {
                stalled = true;
                stalls++;
            }
            uart1_tx_poll();
        }
    }
    return (int)len;
}
