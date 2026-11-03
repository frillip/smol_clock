#include "flash.h"

#include <stddef.h>
#include <xc.h>

#include "../../common/boot_layout.h"

// NVMCON: WREN with the operation.
#define NVMCON_WRITE_DOUBLE_WORD 0x4001U
#define NVMCON_ERASE_PAGE        0x4003U
// The write latches, in table space.
#define LATCH_PAGE 0x00FAU

// The store's pages: reserved so the linker places nothing there, and
// noload so the application's hex never writes them (flash.h).
static __prog__ uint16_t flash_store_area[FLASH_STORE_PAGES * FLASH_PAGE_INSTRUCTIONS]
    __attribute__((space(prog), address(FLASH_STORE_BASE), noload));
// The last page below its configuration words: kept empty, so updates never
// need to erase the page that holds them (flash.h).
static __prog__ uint16_t flash_config_page_free[(FLASH_CONFIG_WORDS - FLASH_CONFIG_PAGE) / 2UL]
    __attribute__((space(prog), address(FLASH_CONFIG_PAGE), noload));

static bool reserved;
static uint32_t (*op_clock)(void);
static void (*resume_hook)(void);
static FlashStats stats;

bool flash_init(void)
{
    // The table address carries more than the program address in its top
    // byte: compare the 24 bits that are the address.
    reserved =
        (__builtin_tbladdress(&flash_store_area[0]) & FLASH_ADDRESS_MASK) == FLASH_STORE_BASE &&
        (__builtin_tbladdress(&flash_config_page_free[0]) & FLASH_ADDRESS_MASK) ==
            FLASH_CONFIG_PAGE;
    return reserved;
}


void flash_set_clock(uint32_t (*new_clock)(void))
{
    op_clock = new_clock;
}


void flash_set_resume_hook(void (*hook)(void))
{
    resume_hook = hook;
}


const FlashStats *flash_stats(void)
{
    return &stats;
}


const char *flash_result_name(FlashResult result)
{
    static const char *const names[] = {"ok", "bad address", "not reserved", "failed"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "?";
}


static bool in_store(uint32_t address, uint32_t alignment)
{
    return address >= FLASH_STORE_BASE && address < FLASH_STORE_END &&
           (address & (alignment - 1UL)) == 0UL;
}


uint16_t flash_read_word(uint32_t address)
{
    uint16_t saved = TBLPAG;
    uint16_t value;

    TBLPAG = (uint16_t)(address >> 16);
    value = __builtin_tblrdl((uint16_t)address);
    TBLPAG = saved;
    return value;
}


// Starts the operation set up in NVMCON, NVMADR and the latches, and waits
// for it with interrupts held off. Returns the ticks it took.
static uint32_t run_operation(void)
{
    uint16_t interrupts = INTCON2bits.GIE;
    uint32_t start;
    uint32_t ticks = 0;

    INTCON2bits.GIE = 0;
    start = op_clock != NULL ? op_clock() : 0UL;
    __builtin_write_NVM();
    while (NVMCONbits.WR)
    {
    }
    if (op_clock != NULL)
    {
        ticks = op_clock() - start;
    }
    if (resume_hook != NULL)
    {
        resume_hook();
    }
    INTCON2bits.GIE = interrupts;
    return ticks;
}


static FlashResult finish(void)
{
    bool failed = NVMCONbits.WRERR != 0U;

    NVMCONbits.WREN = 0;
    if (failed)
    {
        stats.failures++;
        return FLASH_FAILED;
    }
    return FLASH_OK;
}


FlashResult flash_erase_page(uint32_t address)
{
    if (!reserved)
    {
        return FLASH_NOT_RESERVED;
    }
    if (!in_store(address, FLASH_PAGE_ADDRESSES))
    {
        return FLASH_BAD_ADDRESS;
    }
    NVMCON = NVMCON_ERASE_PAGE;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    stats.last_erase_ticks = run_operation();
    if (stats.last_erase_ticks > stats.longest_erase_ticks)
    {
        stats.longest_erase_ticks = stats.last_erase_ticks;
    }
    stats.erases++;
    return finish();
}


// The SPL's pages: the only flash outside the store the application writes.
static bool in_spl(uint32_t address, uint32_t alignment)
{
    return address >= SPL_START && address < SPL_END && (address & (alignment - 1UL)) == 0UL;
}


uint32_t flash_read_instruction(uint32_t address)
{
    uint16_t saved = TBLPAG;
    uint16_t low;
    uint16_t high;

    TBLPAG = (uint16_t)(address >> 16);
    low = __builtin_tblrdl((uint16_t)address);
    high = __builtin_tblrdh((uint16_t)address);
    TBLPAG = saved;
    return (uint32_t)(high & 0x00FFU) << 16 | low;
}


FlashResult flash_spl_erase_page(uint32_t address)
{
    if (!in_spl(address, FLASH_PAGE_ADDRESSES))
    {
        return FLASH_BAD_ADDRESS;
    }
    NVMCON = NVMCON_ERASE_PAGE;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    stats.last_erase_ticks = run_operation();
    if (stats.last_erase_ticks > stats.longest_erase_ticks)
    {
        stats.longest_erase_ticks = stats.last_erase_ticks;
    }
    stats.erases++;
    return finish();
}


FlashResult flash_spl_write(uint32_t address, uint32_t first, uint32_t second)
{
    uint16_t saved = TBLPAG;
    uint32_t ticks;

    if (!in_spl(address, FLASH_DOUBLE_ADDRESSES))
    {
        return FLASH_BAD_ADDRESS;
    }
    NVMCON = NVMCON_WRITE_DOUBLE_WORD;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    TBLPAG = LATCH_PAGE;
    __builtin_tblwtl(0U, (uint16_t)first);
    __builtin_tblwth(0U, (uint16_t)(first >> 16) & 0x00FFU);
    __builtin_tblwtl(2U, (uint16_t)second);
    __builtin_tblwth(2U, (uint16_t)(second >> 16) & 0x00FFU);
    ticks = run_operation();
    TBLPAG = saved;
    if (ticks > stats.longest_write_ticks)
    {
        stats.longest_write_ticks = ticks;
    }
    stats.writes++;
    return finish();
}


FlashResult flash_write_double_word(uint32_t address, uint16_t first, uint16_t second)
{
    uint16_t saved = TBLPAG;
    uint32_t ticks;

    if (!reserved)
    {
        return FLASH_NOT_RESERVED;
    }
    if (!in_store(address, FLASH_DOUBLE_ADDRESSES))
    {
        return FLASH_BAD_ADDRESS;
    }
    NVMCON = NVMCON_WRITE_DOUBLE_WORD;
    NVMADRU = (uint16_t)(address >> 16);
    NVMADR = (uint16_t)address;
    TBLPAG = LATCH_PAGE;
    __builtin_tblwtl(0U, first);
    __builtin_tblwth(0U, 0x00FFU);
    __builtin_tblwtl(2U, second);
    __builtin_tblwth(2U, 0x00FFU);
    ticks = run_operation();
    TBLPAG = saved;
    if (ticks > stats.longest_write_ticks)
    {
        stats.longest_write_ticks = ticks;
    }
    stats.writes++;
    return finish();
}
