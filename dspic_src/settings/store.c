#include "store.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../../common/sml.h"

#define ERASED 0xFFFFU
// Header, length, data, CRC: STORE_RECORD_MAX_BYTES of data at most.
#define RECORD_MAX_WORDS (4U + STORE_RECORD_MAX_BYTES / 2U)

static StoreStatus status;
// Where the latest record of each type starts (0: none), and its length.
static uint16_t record_word[STORE_TYPES];
static uint16_t record_length[STORE_TYPES];

static uint32_t word_address(uint8_t page, uint16_t word)
{
    return (uint32_t)(FLASH_STORE_BASE + (uint32_t)page * FLASH_PAGE_ADDRESSES + 2UL * word);
}


static uint16_t read_word(uint8_t page, uint16_t word)
{
    return flash_read_word(word_address(page, word));
}


static uint16_t crc_word(uint16_t crc, uint16_t word)
{
    uint8_t bytes[2];

    bytes[0] = (uint8_t)word;
    bytes[1] = (uint8_t)(word >> 8);
    return sml_crc(crc, bytes, 2U);
}


// Words a record of this length takes: header, length, data, padding, CRC.
static uint16_t record_words(uint16_t length)
{
    uint16_t words = (uint16_t)(3U + (length + 1U) / 2U);

    return (uint16_t)((words + 1U) & ~1U);
}


static bool header_valid(uint8_t page, uint16_t *generation)
{
    uint16_t words[STORE_HEADER_WORDS];
    uint16_t crc = SML_CRC_INIT;
    uint8_t index;

    for (index = 0; index < STORE_HEADER_WORDS; index++)
    {
        words[index] = read_word(page, index);
    }
    for (index = 0; index < STORE_HEADER_WORDS - 1U; index++)
    {
        crc = crc_word(crc, words[index]);
    }
    *generation = words[2];
    return words[0] == STORE_MAGIC && words[1] == STORE_FORMAT && words[3] == crc;
}


// The record at word: the words it spans, or 0 if it is not one (nothing
// after it can be trusted). intact: its CRC is good.
static uint16_t check_record(uint8_t page, uint16_t word, uint8_t *type, uint16_t *length,
                             bool *intact)
{
    uint16_t mark = read_word(page, word);
    uint16_t words;
    uint16_t crc = SML_CRC_INIT;
    uint16_t index;

    *type = (uint8_t)mark;
    *length = read_word(page, (uint16_t)(word + 1U));
    if ((mark & 0xFF00U) != STORE_RECORD_MARK || *type == 0U || *type == 0xFFU ||
        *length > STORE_RECORD_MAX_BYTES)
    {
        return 0;
    }
    words = record_words(*length);
    if ((uint32_t)word + words > FLASH_PAGE_WORDS)
    {
        return 0;
    }
    for (index = 0; index < words - 1U; index++)
    {
        crc = crc_word(crc, read_word(page, (uint16_t)(word + index)));
    }
    *intact = read_word(page, (uint16_t)(word + words - 1U)) == crc;
    return words;
}


// Reads the page in use: the latest intact record of each type, and where
// the free space starts.
static void scan(void)
{
    uint16_t word = STORE_HEADER_WORDS;

    memset(record_word, 0, sizeof(record_word));
    memset(record_length, 0, sizeof(record_length));
    status.full = false;
    while (word + 2U <= FLASH_PAGE_WORDS)
    {
        uint8_t type;
        uint16_t length;
        bool intact = false;
        uint16_t words;

        if (read_word(status.page, word) == ERASED &&
            read_word(status.page, (uint16_t)(word + 1U)) == ERASED)
        {
            break;
        }
        words = check_record(status.page, word, &type, &length, &intact);
        if (words == 0U)
        {
            status.unreadable = true;
            status.full = true;
            break;
        }
        if (!intact)
        {
            status.torn++;
        }
        else if (type < STORE_TYPES)
        {
            record_word[type] = word;
            record_length[type] = length;
        }
        word = (uint16_t)(word + words);
    }
    status.used_words = word;
}


void store_init(void)
{
    uint16_t generation[FLASH_STORE_PAGES];
    bool valid[FLASH_STORE_PAGES];
    uint8_t page;

    memset(&status, 0, sizeof(status));
    memset(record_word, 0, sizeof(record_word));
    memset(record_length, 0, sizeof(record_length));
    for (page = 0; page < FLASH_STORE_PAGES; page++)
    {
        valid[page] = header_valid(page, &generation[page]);
    }
    if (!valid[0] && !valid[1])
    {
        return;
    }
    // The later generation, across the wrap.
    status.page =
        (uint8_t)(valid[0] && (!valid[1] || (int16_t)(generation[0] - generation[1]) >= 0) ? 0U
                                                                                           : 1U);
    status.generation = generation[status.page];
    status.active = true;
    scan();
}


bool store_read(uint8_t type, uint8_t *data, uint16_t capacity, uint16_t *length)
{
    uint16_t word;
    uint16_t index;

    if (!status.active || type == 0U || type >= STORE_TYPES || record_word[type] == 0U ||
        record_length[type] > capacity)
    {
        return false;
    }
    word = (uint16_t)(record_word[type] + 2U);
    for (index = 0; index < record_length[type]; index++)
    {
        uint16_t value = read_word(status.page, (uint16_t)(word + index / 2U));
        data[index] = (uint8_t)((index & 1U) == 0U ? value : value >> 8);
    }
    *length = record_length[type];
    return true;
}


static bool blank(uint8_t page, uint16_t word, uint16_t words)
{
    uint16_t index;

    for (index = 0; index < words; index++)
    {
        if (read_word(page, (uint16_t)(word + index)) != ERASED)
        {
            return false;
        }
    }
    return true;
}


// Writes words (an even number) from word, a double word at a time in
// order, and reads them back.
static StoreResult write_words(uint8_t page, uint16_t word, const uint16_t *words, uint16_t count)
{
    uint16_t index;

    for (index = 0; index < count; index += 2U)
    {
        status.last_flash = flash_write_double_word(word_address(page, (uint16_t)(word + index)),
                                                    words[index], words[index + 1U]);
        if (status.last_flash != FLASH_OK)
        {
            return STORE_FLASH_FAILED;
        }
    }
    for (index = 0; index < count; index++)
    {
        if (read_word(page, (uint16_t)(word + index)) != words[index])
        {
            return STORE_VERIFY;
        }
    }
    return STORE_OK;
}


// The words of a record. Returns how many.
static uint16_t build_record(uint8_t type, const uint8_t *data, uint16_t length, uint16_t *words)
{
    uint16_t count = record_words(length);
    uint16_t crc = SML_CRC_INIT;
    uint16_t index;

    memset(words, 0, count * sizeof(words[0]));
    words[0] = (uint16_t)(STORE_RECORD_MARK | type);
    words[1] = length;
    for (index = 0; index < length; index++)
    {
        words[2U + index / 2U] |= (uint16_t)((uint16_t)data[index] << ((index & 1U) * 8U));
    }
    for (index = 0; index < count - 1U; index++)
    {
        crc = crc_word(crc, words[index]);
    }
    words[count - 1U] = crc;
    return count;
}


// The latest record of each other type and the new one, into the other
// page, then its header: the switch.
static StoreResult change_page(uint8_t type, const uint8_t *data, uint16_t length)
{
    uint8_t target = status.active ? (uint8_t)(1U - status.page) : 0U;
    uint16_t generation = status.active ? (uint16_t)(status.generation + 1U) : 1U;
    uint16_t new_word[STORE_TYPES];
    uint16_t words[RECORD_MAX_WORDS];
    uint16_t word = STORE_HEADER_WORDS;
    uint16_t count;
    uint8_t other;
    StoreResult result;

    status.last_flash = flash_erase_page(word_address(target, 0U));
    if (status.last_flash != FLASH_OK)
    {
        return STORE_FLASH_FAILED;
    }
    if (!blank(target, 0U, FLASH_PAGE_WORDS))
    {
        return STORE_VERIFY;
    }
    memset(new_word, 0, sizeof(new_word));
    for (other = 1; other < STORE_TYPES; other++)
    {
        uint16_t index;

        if (other == type || record_word[other] == 0U)
        {
            continue;
        }
        count = record_words(record_length[other]);
        for (index = 0; index < count; index++)
        {
            words[index] = read_word(status.page, (uint16_t)(record_word[other] + index));
        }
        result = write_words(target, word, words, count);
        if (result != STORE_OK)
        {
            return result;
        }
        new_word[other] = word;
        word = (uint16_t)(word + count);
    }
    count = build_record(type, data, length, words);
    result = write_words(target, word, words, count);
    if (result != STORE_OK)
    {
        return result;
    }
    new_word[type] = word;
    word = (uint16_t)(word + count);

    words[0] = STORE_MAGIC;
    words[1] = STORE_FORMAT;
    words[2] = generation;
    words[3] = crc_word(crc_word(crc_word(SML_CRC_INIT, words[0]), words[1]), words[2]);
    result = write_words(target, 0U, words, STORE_HEADER_WORDS);
    if (result != STORE_OK)
    {
        return result;
    }

    memcpy(record_word, new_word, sizeof(record_word));
    record_length[type] = length;
    status.active = true;
    status.page = target;
    status.generation = generation;
    status.used_words = word;
    status.full = false;
    status.page_changes++;
    return STORE_OK;
}


bool store_write_erases(uint16_t length)
{
    return !status.active || status.full ||
           (uint32_t)status.used_words + record_words(length) > FLASH_PAGE_WORDS;
}


StoreResult store_write(uint8_t type, const uint8_t *data, uint16_t length)
{
    uint16_t words[RECORD_MAX_WORDS];
    uint16_t count;
    StoreResult result;

    if (type == 0U || type >= STORE_TYPES || length > STORE_RECORD_MAX_BYTES)
    {
        status.last_result = STORE_BAD_ARGUMENT;
        return STORE_BAD_ARGUMENT;
    }
    if (!store_write_erases(length))
    {
        count = build_record(type, data, length, words);
        if (blank(status.page, status.used_words, count))
        {
            result = write_words(status.page, status.used_words, words, count);
            if (result == STORE_OK)
            {
                record_word[type] = status.used_words;
                record_length[type] = length;
                status.used_words = (uint16_t)(status.used_words + count);
                status.appends++;
                status.last_result = STORE_OK;
                return STORE_OK;
            }
            status.failures++;
        }
        // Whatever is left there is passed over when read; nothing more is
        // appended to this page.
        status.full = true;
    }
    result = change_page(type, data, length);
    if (result != STORE_OK)
    {
        status.failures++;
    }
    status.last_result = result;
    return result;
}


const StoreStatus *store_status(void)
{
    return &status;
}


const char *store_result_name(StoreResult result)
{
    static const char *const names[] = {"ok", "bad argument", "flash failed", "verify failed"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "?";
}


void store_print(const char *const names[STORE_TYPES], bool verbose)
{
    uint8_t type;
    bool first = true;

    if (!status.active)
    {
        printf("  Store: EMPTY");
    }
    else
    {
        printf("  Store: %u/%u words%s;", status.used_words, FLASH_PAGE_WORDS,
               status.full ? " (FULL)" : "");
        for (type = 1; type < STORE_TYPES; type++)
        {
            if (record_word[type] != 0U)
            {
                printf("%s %s %u B", first ? "" : ",",
                       names != NULL && names[type] != NULL ? names[type] : "?",
                       record_length[type]);
                first = false;
            }
        }
        if (first)
        {
            printf(" no records");
        }
    }
    if (status.failures != 0U || status.last_result != STORE_OK)
    {
        printf("; %lu FAILURES, last %s (flash %s)", (unsigned long)status.failures,
               store_result_name(status.last_result), flash_result_name(status.last_flash));
    }
    if (status.torn != 0U || status.unreadable)
    {
        printf("; %u TORN%s", status.torn, status.unreadable ? ", UNREADABLE data" : "");
    }
    if (verbose)
    {
        printf("; page %u, generation %u, %lu appends, %lu page changes", status.page,
               status.generation, (unsigned long)status.appends,
               (unsigned long)status.page_changes);
    }
    printf("\r\n");
}
