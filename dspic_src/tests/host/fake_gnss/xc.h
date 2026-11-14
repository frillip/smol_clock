// Host stand-in for <xc.h> for gnss.c: only the UART2 priority fields it sets.
#ifndef FAKE_GNSS_XC_H
#define FAKE_GNSS_XC_H
typedef struct
{
    unsigned U2RXIP : 3, U2TXIP : 3;
} IPC7BITS_t;
typedef struct
{
    unsigned U2EIP : 3;
} IPC16BITS_t;
extern volatile IPC7BITS_t IPC7bits;
extern volatile IPC16BITS_t IPC16bits;

#endif
