// Minimal stand-in for <xc.h> so drivers/uart2.c compiles on the host. Models
// the UART2 SFRs, a 4-deep RX/TX FIFO and the UART2 interrupt bits.
#ifndef FAKE_XC_H
#define FAKE_XC_H
#include <stdint.h>

typedef struct
{
    unsigned BRGH : 1;
    unsigned UARTEN : 1;
} U2MODEBITS;
typedef struct
{
    unsigned URXDA : 1;
    unsigned OERR : 1;
    unsigned FERR : 1;
    unsigned PERR : 1;
    unsigned URXISEL : 2;
    unsigned TRMT : 1;
    unsigned UTXBF : 1;
    unsigned UTXEN : 1;
    unsigned UTXISEL0 : 1;
    unsigned UTXISEL1 : 1;
} U2STABITS;
typedef struct
{
    unsigned U2RXIE : 1;
    unsigned U2TXIE : 1;
    unsigned T4IE : 1;
} IEC1BITS;
typedef struct
{
    unsigned U2EIE : 1;
} IEC4BITS;
typedef struct
{
    unsigned U2RXIF : 1;
    unsigned U2TXIF : 1;
} IFS1BITS;
typedef struct
{
    unsigned U2EIF : 1;
} IFS4BITS;

extern U2MODEBITS U2MODEbits;
extern U2STABITS U2STAbits;
extern IEC1BITS IEC1bits;
extern IEC4BITS IEC4bits;
extern IFS1BITS IFS1bits;
extern IFS4BITS IFS4bits;
extern uint16_t U2BRG;
// Whole-register writes are only ever zero in the driver.
#define U2MODE  (*fake_u2mode_reset())
#define U2STA   (*fake_u2sta_reset())
#define U2RXREG fake_u2rxreg_read()
#define U2TXREG (*fake_u2txreg_slot())
uint16_t *fake_u2mode_reset(void);
uint16_t *fake_u2sta_reset(void);
uint16_t fake_u2rxreg_read(void);
uint16_t *fake_u2txreg_slot(void);

#endif
