// Host stand-in for <xc.h> for drivers/tubes.c. SPI2BUF and the latch pin
// log every access, so the test sees exactly what was written and in what
// order (the driver never reads them back on the host: the receive buffer
// reports empty). Definitions in test_display.c.
#ifndef FAKE_TUBES_XC_H
#define FAKE_TUBES_XC_H
#include <stdint.h>

#define FAKE_LOG_LENGTH 64U
typedef struct
{
    uint16_t values[FAKE_LOG_LENGTH];
    unsigned count;
} FakeLog;
extern FakeLog fake_spi2buf, fake_latch_gpio;

#define SPI2BUF (fake_spi2buf.values[fake_spi2buf.count++ % FAKE_LOG_LENGTH])
#define _LATB12 (fake_latch_gpio.values[fake_latch_gpio.count++ % FAKE_LOG_LENGTH])

extern volatile uint16_t SPI2STAT, SPI2CON1, SPI2CON2;
extern volatile uint16_t OC3CON1, OC3CON2, OC3R, OC3RS;
extern volatile uint16_t _LATB11, _RB10;
typedef struct
{
    unsigned SPITBF : 1, SRXMPT : 1, SRMPT : 1, SPIROV : 1, SPIBEC : 3;
} SPI2STATBITS_t;
extern volatile SPI2STATBITS_t SPI2STATbits;
typedef struct
{
    unsigned SPI2IF : 1;
} IFS2BITS_t;
typedef struct
{
    unsigned SPI2IE : 1;
} IEC2BITS_t;
extern volatile IFS2BITS_t IFS2bits;
extern volatile IEC2BITS_t IEC2bits;

#endif
