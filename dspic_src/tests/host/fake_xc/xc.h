/* Host stand-in for <xc.h> for the timing modules (epoch_timer.c,
 * epoch_output.c, timing.c). Whole registers and bit fields are separate
 * plain variables: each module touches a register only one way, so they
 * never need to agree. Definitions live in fake_sfr.c. */
#ifndef FAKE_XC_H
#define FAKE_XC_H
#include <stdint.h>
#ifndef SFR
#define SFR extern
#endif
#define __builtin_disi(x) ((void)0)

SFR volatile uint16_t PR2, PR3, PR4, TMR2, TMR3, TMR3HLD, TMR4, T2CON, T4CON;
SFR volatile uint16_t OC1CON1, OC1CON2, OC1R, OC1RS;
SFR volatile uint16_t _LATA10, _RA10, _TRISA10, _RB10, _LATB13, _TRISB13;

typedef struct
{
    unsigned T32 : 1, TCKPS : 2, TCS : 1, TON : 1;
} T2CONBITS_t;
typedef struct
{
    unsigned TON : 1;
} T4CONBITS_t;
typedef struct
{
    unsigned T3IE : 1, OC1IE : 1;
} IEC0BITS_t;
typedef struct
{
    unsigned T4IE : 1;
} IEC1BITS_t;
typedef struct
{
    unsigned T3IF : 1, OC1IF : 1;
} IFS0BITS_t;
typedef struct
{
    unsigned T4IF : 1;
} IFS1BITS_t;
typedef struct
{
    unsigned T3IP : 3;
} IPC2BITS_t;
typedef struct
{
    unsigned T4IP : 3;
} IPC6BITS_t;

SFR volatile T2CONBITS_t T2CONbits;
SFR volatile T4CONBITS_t T4CONbits;
SFR volatile IEC0BITS_t IEC0bits;
SFR volatile IEC1BITS_t IEC1bits;
SFR volatile IFS0BITS_t IFS0bits;
SFR volatile IFS1BITS_t IFS1bits;
SFR volatile IPC2BITS_t IPC2bits;
SFR volatile IPC6BITS_t IPC6bits;

#endif
