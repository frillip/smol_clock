// Host stand-in for <libpic30.h>: the delays do nothing.
#ifndef FAKE_LIBPIC30_H
#define FAKE_LIBPIC30_H
#define __delay_us(x) ((void)0)
#define __delay_ms(x) ((void)0)
#endif
