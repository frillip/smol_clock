/*
 * The configuration words. They live in the last page, which only a
 * programmer writes, so they go with the IPL: programmed once, at first
 * installation, with it. The SPL's and the application's hex files carry
 * none (tools/make_image.py drops any it finds). As V2+ 0.3.1 had them in
 * dspic_src/platform/system.c, but for the watchdog postscaler: PS1024.
 */
#include <xc.h>

// FICD
// ICD Communication Channel Select bits: Communicate on PGEC2 and PGED2
#pragma config ICS = PGD2
// JTAG Enable bit: JTAG is disabled
#pragma config JTAGEN = OFF

// FPOR
// Alternate I2C1 pins: I2C1 mapped to ASDA1/ASCL1 (RB9/RB8), where the RTC
// and sensors are wired. As V2. OFF would put I2C1 on SDA1/SCL1 instead.
#pragma config ALTI2C1 = ON
// Alternate I2C2 pins: I2C2 mapped to SDA2/SCL2 pins
#pragma config ALTI2C2 = OFF
// Watchdog Window Select bits: WDT Window is 25% of WDT period
#pragma config WDTWIN = WIN25

// FWDT
// Watchdog Timer Postscaler bits: 1:1024. With PR128 on the 32 kHz LPRC,
// 4.1 s nominal, 3.905 s measured: the period every stage lives with.
#pragma config WDTPOST = PS1024
// Watchdog Timer Prescaler bit: 1:128
#pragma config WDTPRE = PR128
// PLL Lock Enable bit: Clock switch to PLL source will wait until the PLL lock signal is valid.
#pragma config PLLKEN = ON
// Watchdog Timer Window Enable bit: Watchdog Timer in Non-Window mode
#pragma config WINDIS = OFF
// Watchdog Timer Enable bit: Watchdog timer enabled/disabled by user software
#pragma config FWDTEN = OFF

// FOSC
// Primary Oscillator Mode Select bits: EC (External Clock) Mode
#pragma config POSCMD = EC
// OSC2 Pin Function bit: OSC2 is general purpose digital I/O pin
#pragma config OSCIOFNC = ON
// Peripheral pin select configuration: Allow multiple reconfigurations
#pragma config IOL1WAY = OFF
// Clock Switching Mode bits: Clock switching is enabled,Fail-safe Clock Monitor is disabled
#pragma config FCKSM = CSECMD

// FOSCSEL
// Oscillator Source Selection: FRC
#pragma config FNOSC = FRC
// Two-speed Oscillator Start-up Enable bit: Start up with user-selected oscillator source
#pragma config IESO = OFF

// FGS
// General Segment Write-Protect bit: General Segment may be written
#pragma config GWRP = OFF
// General Segment Code-Protect bit: General Segment Code protect is Disabled
#pragma config GCP = OFF
