#include "fake_flash.h"

#include <stdlib.h>
#include <string.h>

uint16_t fake_flash_words[FLASH_STORE_PAGES][FLASH_PAGE_WORDS];
uint32_t fake_flash_violations;
uint32_t fake_flash_erases[FLASH_STORE_PAGES];
long fake_flash_cut_after = -1;
jmp_buf fake_flash_reset;
uint32_t fake_flash_resumes;
int fake_flash_fail;

static uint8_t written[FLASH_STORE_PAGES][FLASH_PAGE_WORDS / 2U];
static FlashStats stats;
static void (*resume_hook)(void);
static unsigned random_state = 1U;

static unsigned next_random(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return (random_state >> 8) & 0xFFFFU;
}


void fake_flash_seed(unsigned seed)
{
    random_state = seed;
}


void fake_flash_wipe(uint16_t value)
{
    uint8_t page;
    uint16_t word;

    for (page = 0; page < FLASH_STORE_PAGES; page++)
    {
        for (word = 0; word < FLASH_PAGE_WORDS; word++)
        {
            fake_flash_words[page][word] = value;
        }
    }
    memset(written, 0, sizeof(written));
}


// True if the power is cut at this operation.
static int cut_now(void)
{
    if (fake_flash_cut_after < 0)
    {
        return 0;
    }
    if (fake_flash_cut_after == 0)
    {
        fake_flash_cut_after = -1;
        return 1;
    }
    fake_flash_cut_after--;
    return 0;
}


static int locate(uint32_t address, uint32_t alignment, uint8_t *page, uint16_t *word)
{
    if (address < FLASH_STORE_BASE || address >= FLASH_STORE_END || (address % alignment) != 0U)
    {
        return 0;
    }
    *page = (uint8_t)((address - FLASH_STORE_BASE) / FLASH_PAGE_ADDRESSES);
    *word = (uint16_t)(((address - FLASH_STORE_BASE) % FLASH_PAGE_ADDRESSES) / 2U);
    return 1;
}


bool flash_init(void)
{
    return true;
}


void flash_set_clock(uint32_t (*clock)(void))
{
    (void)clock;
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
    return result == FLASH_OK ? "ok" : "failed";
}


uint16_t flash_read_word(uint32_t address)
{
    uint8_t page;
    uint16_t word;

    if (!locate(address, 2U, &page, &word))
    {
        abort(); // the store reads only its own pages
    }
    return fake_flash_words[page][word];
}


FlashResult flash_erase_page(uint32_t address)
{
    uint8_t page;
    uint16_t word;
    uint16_t index;

    if (!locate(address, FLASH_PAGE_ADDRESSES, &page, &word))
    {
        return FLASH_BAD_ADDRESS;
    }
    if (fake_flash_fail)
    {
        return FLASH_FAILED;
    }
    if (cut_now())
    {
        // Part erased: a random run of words set, the rest as they were.
        uint16_t from = (uint16_t)(next_random() % FLASH_PAGE_WORDS);
        uint16_t to = (uint16_t)(from + next_random() % (FLASH_PAGE_WORDS - from));
        for (index = from; index < to; index++)
        {
            fake_flash_words[page][index] = 0xFFFFU;
        }
        longjmp(fake_flash_reset, 1);
    }
    for (index = 0; index < FLASH_PAGE_WORDS; index++)
    {
        fake_flash_words[page][index] = 0xFFFFU;
    }
    memset(written[page], 0, sizeof(written[page]));
    fake_flash_erases[page]++;
    stats.erases++;
    if (resume_hook != NULL)
    {
        fake_flash_resumes++;
        resume_hook();
    }
    return FLASH_OK;
}


FlashResult flash_write_double_word(uint32_t address, uint16_t first, uint16_t second)
{
    uint8_t page;
    uint16_t word;

    if (!locate(address, FLASH_DOUBLE_ADDRESSES, &page, &word))
    {
        return FLASH_BAD_ADDRESS;
    }
    if (fake_flash_fail)
    {
        return FLASH_FAILED;
    }
    // Written since the last erase, or never erased at all.
    if (written[page][word / 2U] || fake_flash_words[page][word] != 0xFFFFU ||
        fake_flash_words[page][word + 1U] != 0xFFFFU)
    {
        fake_flash_violations++;
    }
    if (cut_now())
    {
        // Some of the bits programmed.
        fake_flash_words[page][word] &= (uint16_t)(first | next_random());
        fake_flash_words[page][word + 1U] &= (uint16_t)(second | next_random());
        written[page][word / 2U] = 1;
        longjmp(fake_flash_reset, 1);
    }
    fake_flash_words[page][word] &= first;
    fake_flash_words[page][word + 1U] &= second;
    written[page][word / 2U] = 1;
    stats.writes++;
    return FLASH_OK;
}
