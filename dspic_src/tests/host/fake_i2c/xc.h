/* Host stand-in for <xc.h> for the I2C1 master (drivers/i2c1.c): I2C1CON
 * and I2C1STAT as bitfields over one word each, in the dsPIC's bit order,
 * a transmit register that tells the model when it is written, the MI2C1
 * interrupt bits, and RB8/RB9 for bus recovery. Definitions and the bus
 * model are in test_i2c.c. */
#ifndef FAKE_I2C_XC_H
#define FAKE_I2C_XC_H
#include <stdint.h>

typedef struct
{
    unsigned SEN : 1, RSEN : 1, PEN : 1, RCEN : 1, ACKEN : 1, ACKDT : 1, STREN : 1, GCEN : 1;
    unsigned SMEN : 1, DISSLW : 1, A10M : 1, IPMIEN : 1, SCLREL : 1, I2CSIDL : 1, : 1, I2CEN : 1;
} I2C1CONBITS;
typedef struct
{
    unsigned TBF : 1, RBF : 1, R_W : 1, S : 1, P : 1, D_A : 1, I2COV : 1, IWCOL : 1;
    unsigned ADD10 : 1, GCSTAT : 1, BCL : 1, : 3, TRSTAT : 1, ACKSTAT : 1;
} I2C1STATBITS;
typedef struct
{
    unsigned MI2C1IF : 1;
} IFS1BITS;
typedef struct
{
    unsigned MI2C1IE : 1;
} IEC1BITS;
typedef struct
{
    unsigned MI2C1IP : 3;
} IPC4BITS;

extern volatile I2C1CONBITS I2C1CONbits;
extern volatile I2C1STATBITS I2C1STATbits;
extern volatile IFS1BITS IFS1bits;
extern volatile IEC1BITS IEC1bits;
extern volatile IPC4BITS IPC4bits;
extern volatile uint16_t I2C1BRG;
extern volatile uint16_t I2C1RCV;
#define I2C1CON  (*(volatile uint16_t *)&I2C1CONbits)
#define I2C1STAT (*(volatile uint16_t *)&I2C1STATbits)
volatile uint16_t *fake_i2c_trn(void); // marks a byte to send
#define I2C1TRN (*fake_i2c_trn())

extern volatile unsigned _LATB8, _LATB9, _TRISB8, _TRISB9;
int fake_scl(void);
int fake_sda(void);
#define _RB8 fake_scl()
#define _RB9 fake_sda()

#define __builtin_disi(x)             ((void)0)
#define __builtin_enable_interrupts() ((void)0)
#endif
