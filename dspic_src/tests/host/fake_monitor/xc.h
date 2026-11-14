/* Host stand-in for <xc.h> for the PPS watch (pps_monitor.c): the change
 * notification registers, and the two PPS pins read through
 * fake_pin_read() so a test can make an edge arrive in the middle of the
 * handler. Definitions in test_monitor.c. */
#ifndef FAKE_MONITOR_XC_H
#define FAKE_MONITOR_XC_H
#include <stdint.h>
#define __builtin_disi(x) ((void)0)

typedef struct
{
    unsigned CNIF : 1;
} IFS1BITS_t;
typedef struct
{
    unsigned CNIE : 1;
} IEC1BITS_t;
typedef struct
{
    unsigned CNIP : 3;
} IPC4BITS_t;
typedef struct
{
    unsigned CNIEA8 : 1;
} CNENABITS_t;
typedef struct
{
    unsigned CNIEB15 : 1;
} CNENBBITS_t;

extern volatile IFS1BITS_t IFS1bits;
extern volatile IEC1BITS_t IEC1bits;
extern volatile IPC4BITS_t IPC4bits;
extern volatile CNENABITS_t CNENAbits;
extern volatile CNENBBITS_t CNENBbits;

int fake_pin_read(int pin); // 0 RA8 (GNSS PPS), 1 RB15 (ESP PPS)
#define _RA8  fake_pin_read(0)
#define _RB15 fake_pin_read(1)
#endif
