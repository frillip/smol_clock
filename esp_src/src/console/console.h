/*
 * Console input lines and console output, for both the serial console and
 * telnet.
 *
 * Lines end at CR, LF or CR LF (one line, not two). Backspace and DEL erase.
 * A line too long for the buffer is discarded with a new prompt.
 *
 * Text from the PIC (STAT-DEBUG, or raw text before it is in SML mode) and
 * the ESP's own log lines go to the serial console and to a connected telnet
 * client.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#define CONSOLE_LINE_BYTES 128

typedef struct
{
    char text[CONSOLE_LINE_BYTES];
    uint8_t length;
    char previous; // the character before, to treat CR LF as one end of line
} ConsoleLine;

// Reads what input has; true when a line is complete in line->text (then
// call console_line_clear() once it has been used).
bool console_read_line(ConsoleLine *line, Stream *input, bool echo);
void console_line_clear(ConsoleLine *line);

// To the serial console and any telnet client.
void console_write_text(const uint8_t *text, size_t length);
void console_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
// The same as a Stream, for code that reports to one (writes only).
Stream *console_stream(void);

#endif // CONSOLE_H
