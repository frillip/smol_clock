/*
 * The SPL's hardware (spl_port.h): the dsPIC33EP256GP504 on V2, polled.
 * No interrupt is ever enabled.
 *
 *   clock     FRC to the primary PLL, 40 MIPS, as the application's; the
 *             FRC's PLL instead if the external clock does not start
 *   time      Timer2/3 as one 32-bit timer, 1:64: 625 kHz, counted into ms
 *   UART1     909091 baud 8N1 (BRGH, divisor 11), RC7 TX (RP55), RC6 RX
 *             (RPI54), as the application's link to the ESP
 *   LED       RA10
 *   watchdog  on from the start (PR128, PS1024: 4.1 s), off for the hand-over
 *   flash     page erase and double-word writes, the guard first
 *   CRC-32    the IPL's service at IPL_SERVICES
 */
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>

#include "spl_port.h"

#define TICKS_PER_MS 625U // FCY 40 MHz / 64

#define NVMCON_WRITE_DOUBLE_WORD 0x4001U
#define NVMCON_ERASE_PAGE        0x4003U
#define LATCH_PAGE               0x00FAU

#define RING_SIZE 256U // a power of two
#define RING_MASK (RING_SIZE - 1U)

#define NOSC_PRIPLL  0x03U
#define NOSC_FRCPLL  0x01U
#define SWITCH_POLLS 20000U // polls of OSWEN on the FRC: some 30 ms
#define OSCTUN_FALLBACK 0x3CU // TUN -4: the FRC 0.188% slow

// Reset values, to leave the chip as the application expects it.
#define CLKDIV_RESET 0x3040U
#define PLLFBD_RESET 0x0030U

#define JUMP_TO_APP() __asm__ volatile("goto 0x002000")
typedef char jump_app_check[(APP_START == 0x002000UL) ? 1 : -1];

static uint8_t ring[RING_SIZE];
static uint16_t ring_head;
static uint16_t ring_tail;
static uint32_t last_ticks;
static uint32_t ticks_left; // not yet a whole ms
static uint32_t ms;

// Switches to nosc and waits for it and the PLL's lock, some 30 ms at most
// (a switch to a clock that is not there never completes). False if it did
// not: the switch abandoned, still on the FRC.
static bool clock_switch(uint8_t nosc)
{
    uint16_t polls;

    __builtin_write_OSCCONH(nosc);
    __builtin_write_OSCCONL((uint8_t)(OSCCON | 0x01U));
    for (polls = 0; OSCCONbits.OSWEN != 0U; polls++)
    {
        if (polls >= SWITCH_POLLS)
        {
            __builtin_write_OSCCONL((uint8_t)(OSCCON & 0xFEU));
            return false;
        }
    }
    for (polls = 0; OSCCONbits.LOCK != 1U; polls++)
    {
        if (polls >= SWITCH_POLLS)
        {
            return false;
        }
    }
    return true;
}


static void clock_init(void)
{
    // As the application's clock_init(): EC 40 MHz, PLLPRE 8, PLLDIV 62
    // (M 64), PLLPOST 4: Fosc 80 MHz, 40 MIPS.
    CLKDIV = 0x3146;
    PLLFBD = 0x3E;
    if (clock_switch(NOSC_PRIPLL))
    {
        return;
    }
    // No external clock: the FRC, PLLPRE 2, PLLDIV 85 (M 87), PLLPOST 4:
    // Fosc 80.15 MHz, trimmed by OSCTUN -4 (-0.188%) to 80.00 MHz nominal.
    OSCTUN = OSCTUN_FALLBACK;
    CLKDIV = 0x3140;
    PLLFBD = 0x55;
    (void)clock_switch(NOSC_FRCPLL);
}


void spl_port_init(void)
{
    ClrWdt();
    RCONbits.SWDTEN = 1;

    _LATA10 = 0; // the status LED
    _TRISA10 = 0;
    _LATC7 = 1; // UART1 TX idles high
    _TRISC7 = 0;
    _TRISC6 = 1;

    clock_init();

    __builtin_write_OSCCONL((uint8_t)(OSCCON & 0xBF)); // PPS unlocked
    RPOR5bits.RP55R = 0x0001;                          // RC7 <- U1TX
    RPINR18bits.U1RXR = 0x0036;                        // RC6 -> U1RX

    T2CON = 0;
    T3CON = 0;
    TMR3 = 0;
    TMR2 = 0;
    PR2 = 0xFFFF;
    PR3 = 0xFFFF;
    T2CON = 0x8028; // TON, 1:64, T32

    U1MODE = 0x0008; // BRGH
    U1STA = 0;
    U1BRG = 10; // 40 MHz / (4 * 11) = 909091 baud
    U1MODEbits.UARTEN = 1;
    U1STAbits.UTXEN = 1;
}


uint32_t spl_port_ms(void)
{
    uint16_t low = TMR2; // latches TMR3 into TMR3HLD
    uint32_t now = (uint32_t)TMR3HLD << 16 | low;

    ticks_left += now - last_ticks;
    last_ticks = now;
    while (ticks_left >= TICKS_PER_MS)
    {
        ticks_left -= TICKS_PER_MS;
        ms++;
    }
    return ms;
}


void spl_port_service(void)
{
    ClrWdt();
    if (U1STAbits.OERR)
    {
        U1STAbits.OERR = 0; // empties the FIFO: frames may be lost, never the UART
    }
    while (U1STAbits.URXDA)
    {
        uint8_t byte = (uint8_t)U1RXREG;
        uint16_t next = (uint16_t)((ring_head + 1U) & RING_MASK);

        if (next != ring_tail)
        {
            ring[ring_head] = byte;
            ring_head = next;
        }
    }
}


bool spl_port_rx(uint8_t *byte)
{
    if (ring_tail == ring_head)
    {
        return false;
    }
    *byte = ring[ring_tail];
    ring_tail = (uint16_t)((ring_tail + 1U) & RING_MASK);
    return true;
}


void spl_port_tx(const uint8_t *data, uint16_t length)
{
    while (length-- != 0U)
    {
        while (U1STAbits.UTXBF)
        {
            spl_port_service();
        }
        U1TXREG = *data++;
    }
}


uint32_t spl_port_flash_read(uint32_t address)
{
    uint16_t saved = TBLPAG;
    uint16_t low;
    uint16_t high;

    TBLPAG = (uint16_t)(address >> 16);
    low = (uint16_t)__builtin_tblrdl((uint16_t)address);
    high = (uint16_t)__builtin_tblrdh((uint16_t)address);
    TBLPAG = saved;
    return (uint32_t)(high & 0x00FFU) << 16 | low;
}


// Starts the operation set up in NVMCON, NVMADR and the latches, waits for
// it (the CPU stalls meanwhile) and returns whether it succeeded.
static bool run_operation(void)
{
    bool ok;

    __builtin_write_NVM();
    while (NVMCONbits.WR)
    {
    }
    ok = NVMCONbits.WRERR == 0U;
    NVMCONbits.WREN = 0;
    return ok;
}


bool spl_port_flash_erase(uint32_t address)
{
    if (!spl_may_erase(address))
    {
        return false;
    }
    ClrWdt();
    NVMCON = NVMCON_ERASE_PAGE;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    return run_operation();
}


bool spl_port_flash_write(uint32_t address, uint32_t first, uint32_t second)
{
    uint16_t saved = TBLPAG;
    bool ok;

    if (!spl_may_write(address))
    {
        return false;
    }
    NVMCON = NVMCON_WRITE_DOUBLE_WORD;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    TBLPAG = LATCH_PAGE;
    __builtin_tblwtl(0U, (uint16_t)first);
    __builtin_tblwth(0U, (uint16_t)(first >> 16));
    __builtin_tblwtl(2U, (uint16_t)second);
    __builtin_tblwth(2U, (uint16_t)(second >> 16));
    ok = run_operation();
    TBLPAG = saved;
    return ok;
}


uint32_t spl_port_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions)
{
    return ipl_crc32_flash(crc, address, instructions);
}


void spl_port_led(bool on)
{
    _LATA10 = on ? 1 : 0;
}


static void flush(void)
{
    while (!U1STAbits.TRMT)
    {
        ClrWdt();
    }
}


void spl_port_hand_over(volatile BootMailbox *mailbox)
{
    flush();

    U1MODE = 0;
    U1STA = 0;
    U1BRG = 0;
    T2CON = 0;
    T3CON = 0;
    TMR2 = 0;
    TMR3 = 0;
    PR2 = 0xFFFF;
    PR3 = 0xFFFF;
    RPOR5bits.RP55R = 0; // PPS is still unlocked, as reset leaves it
    RPINR18bits.U1RXR = 0;
    // The one pin not left as reset leaves it: UART1 TX stays driven high,
    // idle, so the line to the ESP does not float, and read as noise there,
    // while the application's C start-up clears RAM on FRC. Its board_init()
    // drives it high too.
    _LATC7 = 1;
    _TRISC7 = 0;
    _LATA10 = 0;
    _TRISA10 = 1;

    // Back to FRC, the PLL settings as reset leaves them.
    __builtin_write_OSCCONH((uint8_t)0x00);
    __builtin_write_OSCCONL((uint8_t)(OSCCON | 0x01));
    while (OSCCONbits.OSWEN != 0)
    {
    }
    CLKDIV = CLKDIV_RESET;
    OSCTUN = 0; // untrimmed, as reset leaves it
    PLLFBD = PLLFBD_RESET;

    ClrWdt();
    RCONbits.SWDTEN = 0;
    mailbox->running = MAILBOX_RUNNING_VALUE; // traps go to the application from here
    JUMP_TO_APP();
    for (;;)
    {
    }
}


void spl_port_reset(void)
{
    flush();
    __asm__ volatile("reset");
    for (;;)
    {
    }
}
