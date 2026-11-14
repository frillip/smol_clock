/* Host stand-in for <xc.h> for uart1.c and console.c only. The UART1
 * registers are a behavioural model (see test_console.c): every access to
 * U1STAbits advances simulated time, so the transmit FIFO drains and the
 * TX interrupt can fire while foreground code spins on the status register,
 * as it would on the device. */
#ifndef FAKE_UART_XC_H
#define FAKE_UART_XC_H
#include <stdint.h>

typedef struct
{
    unsigned U1RXIE : 1;
    unsigned U1TXIE : 1;
} IEC0BITS_t;
typedef struct
{
    unsigned U1EIE : 1;
} IEC4BITS_t;
typedef struct
{
    unsigned U1RXIF : 1;
    unsigned U1TXIF : 1;
} IFS0BITS_t;
typedef struct
{
    unsigned U1EIF : 1;
} IFS4BITS_t;
typedef struct
{
    unsigned U1RXIP : 3;
} IPC2BITS_t;
typedef struct
{
    unsigned U1TXIP : 3;
} IPC3BITS_t;
typedef struct
{
    unsigned U1EIP : 3;
} IPC16BITS_t;
typedef struct
{
    unsigned BRGH : 1;
    unsigned UARTEN : 1;
} U1MODEBITS_t;
typedef struct
{
    unsigned URXDA : 1;
    unsigned OERR : 1;
    unsigned FERR : 1;
    unsigned PERR : 1;
    unsigned TRMT : 1;
    unsigned UTXBF : 1;
    unsigned UTXEN : 1;
    unsigned UTXISEL0 : 1;
    unsigned UTXISEL1 : 1;
    unsigned URXISEL : 2;
} U1STABITS_t;

extern volatile IEC0BITS_t IEC0bits;
extern volatile IEC4BITS_t IEC4bits;
extern volatile IFS0BITS_t IFS0bits;
extern volatile IFS4BITS_t IFS4bits;
extern volatile IPC2BITS_t IPC2bits;
extern volatile IPC3BITS_t IPC3bits;
extern volatile IPC16BITS_t IPC16bits;
extern volatile U1MODEBITS_t U1MODEbits;
extern volatile uint16_t U1MODE, U1STA, U1BRG, U1RXREG;

volatile U1STABITS_t *fake_u1sta(void);
volatile uint16_t *fake_u1txreg(void);
#define U1STAbits (*fake_u1sta())
#define U1TXREG   (*fake_u1txreg())

#endif
