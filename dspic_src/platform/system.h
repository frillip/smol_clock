#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include <stdbool.h>

#include "clock.h" // FCY: must precede <libpic30.h>, whose __delay_us()
                   // and __delay_ms() macros exist only if FCY is set
#include <libpic30.h>
#include <xc.h>
#include "board.h"

void system_init(void);

void delay_ms(uint16_t milliseconds);
void delay_us(uint16_t microseconds);

// RCON as it was at start-up (bit names as SML_RESET_*): why the PIC reset.
uint16_t system_reset_cause(void);

// The external clock did not start (the switch to it never completed):
// running from the FRC's PLL instead, at about the same rate but only as
// accurate as the FRC (STAT-SYS SML_SYS_CLOCK_FALLBACK). The Fail-safe Clock
// Monitor turns a clock that stops later into an oscillator-fail trap, a
// reset, and this.
bool system_clock_fallback(void);

// Software reset (RESET instruction): restarts from the reset vector.
void system_reset(void) __attribute__((noreturn));

#endif // SYSTEM_H
