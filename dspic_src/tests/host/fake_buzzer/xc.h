// Host stand-in for <xc.h> for drivers/buzzer.c: Timer5 and the buzzer
// pin as plain variables. Definitions in test_buzzer.c.
#ifndef FAKE_BUZZER_XC_H
#define FAKE_BUZZER_XC_H
#include <stdint.h>

extern volatile uint16_t T5CON, TMR5, PR5;
extern volatile unsigned _LATB7;
typedef struct
{
    unsigned TON : 1, TCKPS : 2;
} T5CONBITS_t;
typedef struct
{
    unsigned T5IE : 1;
} IEC1BITS_t;
typedef struct
{
    unsigned T5IF : 1;
} IFS1BITS_t;
typedef struct
{
    unsigned T5IP : 3;
} IPC7BITS_t;
extern volatile T5CONBITS_t T5CONbits;
extern volatile IEC1BITS_t IEC1bits;
extern volatile IFS1BITS_t IFS1bits;
extern volatile IPC7BITS_t IPC7bits;
#endif
