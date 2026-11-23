/*
 * printf, snprintf, sprintf and vsnprintf of our own, for the conversions
 * the firmware uses.
 *
 * Why: XC16 2.x's libc (Microchip's musl) converts every integer through a
 * 64-bit divide per digit, and its 64-bit divide (__udivmoddi3, libm's
 * divmoddi3.c) is C: 64 iterations of multi-word shifts and calls. Timed on
 * the clock (2026-11-23): 470 us a digit, so a %lu of 10 digits took 4.7 ms,
 * the time zone's printout (about 230 digits) 113 ms, and pic -v about
 * half a second. Smart-IO's integer-only variants are no faster. Here a
 * 32-bit value is converted with the dsPIC's 32/16 hardware divide, two per
 * digit, about 1 us.
 *
 * What: %s %c %d %i %u %x %X %%, with the l length (long) and h/hh
 * (accepted, as int), flags - + space 0 #, a width and a precision, each a
 * number or *. ll is accepted and correct, but goes through the library's
 * 64-bit divide (slow: avoid it). Anything else (floats, %p, %n) prints the
 * conversion as written, so a missing case shows rather than misbehaves.
 * Output and return values as the C standard's.
 *
 * printf collects a line (FORMAT_LINE_BYTES) and hands it to write() at its
 * end, when full, or at format_flush() (the console calls it every main-
 * loop pass: console_push()).
 *
 * On XC16 this header maps printf, snprintf, sprintf and vsnprintf to these;
 * every file that prints includes it after <stdio.h>. The host tests keep
 * the C library's (and test these against it, tests/host/test_format.c).
 */
#ifndef FORMAT_H
#define FORMAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#define FORMAT_LINE_BYTES 128U

int format_vsnprintf(char *text, size_t size, const char *format, va_list args);
int format_snprintf(char *text, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int format_sprintf(char *text, const char *format, ...) __attribute__((format(printf, 2, 3)));
int format_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int format_vprintf(const char *format, va_list args);
// What printf holds, not yet ended by a line end, to write().
void format_flush(void);

#if defined(__XC16__) && !defined(FORMAT_NO_MAPPING)
#define printf format_printf
#define snprintf format_snprintf
#define sprintf format_sprintf
#define vsnprintf format_vsnprintf
#define vprintf format_vprintf
#endif

#endif // FORMAT_H
