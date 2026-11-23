/*
 * Debug console on UART1: routes the C library's stdout and stderr (printf,
 * puts, putchar) to the UART1 driver. UART1 is the link to the ESP32, which
 * bridges it to a serial terminal. Until the ESP speaks SML the text goes out
 * raw; once it does (esp/esp_link.h), the link installs a sink and the text
 * travels inside SML-STAT-DEBUG frames, between the binary messages.
 *
 * XC16's libc sends all stream output through write(); console.c provides
 * that function in the .libc.write section, which replaces the library's
 * default (the simulator's UART). Nothing else is needed for the existing
 * printf calls to work.
 *
 * printf is our own (platform/format.h: the library's converts each digit
 * with a 64-bit software divide, 470 us a digit), and hands write() a line
 * at a time. Text without a line end waits in its buffer until
 * console_push() (the main loop calls it every pass), console_flush(), a
 * sink change, or the buffer filling.
 *
 * Output policy: write() blocks until every byte is queued. The 1 KB TX ring
 * absorbs the per-second status burst, so in normal running it never waits.
 * If it does wait, it moves bytes into the UART itself (uart1_tx_poll), so it
 * cannot deadlock when called with the UART interrupt held off, for example
 * inside a __builtin_disi() window. Interrupts are never disabled by it, so
 * the epoch timing runs undisturbed while it waits.
 *
 * Foreground use only: do not printf from an interrupt handler.
 */
#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
#include <stdbool.h>

// 921600 (the V2+ ESP link): BRG 10, actual 909091 baud, -1.36 %. 1000000
// would be exact here (BRG 9) and on the ESP32's 80 MHz UART clock.
#ifndef CONSOLE_BAUD_RATE
#define CONSOLE_BAUD_RATE 921600UL
#endif
// Below the timing interrupts (4) and alongside the captures (1).
#define CONSOLE_INTERRUPT_PRIORITY 1

// Sets the UART1 interrupt priorities and starts the UART. Output written
// before this is discarded. Returns false if the baud rate is unreachable.
bool console_init(uint32_t instruction_hz);

// Text to the console, as printf would send it: to the sink if one is set,
// else to the UART. Blocks only if the TX ring is full.
void console_write(const char *text, uint16_t length);

// Times write() had to wait for ring space, for diagnostics.
uint32_t console_stalls(void);
// Hands what printf holds to write(): text not ended by a line end.
void console_push(void);
// Bytes written, and write() calls, since start: how the C library hands
// its output over (a call per character, or per printf).
void console_counts(uint32_t *bytes, uint32_t *calls);

// From now on, hand all console text to sink instead of the UART. NULL goes
// back to raw text. The sink must not printf.
typedef void (*ConsoleSink)(const uint8_t *text, uint16_t length);
void console_set_sink(ConsoleSink sink);

// Waits until everything queued has left the pin, e.g. before a reset.
void console_flush(void);

#endif
