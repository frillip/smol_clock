/*
 * The settings store (settings/store.c) against a model of the flash with
 * its programming rules (fake_flash): reading back across restarts, page
 * changes, wear, and power cut at every point of a write.
 */
#include <stdio.h>
#include <string.h>

#include "../../settings/store.h"
#include "../../../common/sml.h"
#include "fake_flash/fake_flash.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

// What each type should read back as: committed[type], length 0 = none.
typedef struct
{
    uint8_t data[STORE_RECORD_MAX_BYTES];
    uint16_t length;
    int present;
} Expected;

static Expected committed[STORE_TYPES];
static unsigned random_state = 7U;

static unsigned next_random(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return (random_state >> 8) & 0xFFFFU;
}


static void make_record(Expected *record, uint16_t length)
{
    uint16_t index;

    record->length = length;
    record->present = 1;
    for (index = 0; index < length; index++)
    {
        record->data[index] = (uint8_t)next_random();
    }
}


static int reads_as(uint8_t type, const Expected *expected)
{
    uint8_t data[STORE_RECORD_MAX_BYTES];
    uint16_t length = 0;
    bool found = store_read(type, data, sizeof(data), &length);

    if (!expected->present)
    {
        return !found;
    }
    return found && length == expected->length && memcmp(data, expected->data, length) == 0;
}


static int all_read_as_committed(void)
{
    uint8_t type;

    for (type = 1; type < STORE_TYPES; type++)
    {
        if (!reads_as(type, &committed[type]))
        {
            return 0;
        }
    }
    return 1;
}


static void fresh(uint16_t fill)
{
    fake_flash_wipe(fill);
    memset(committed, 0, sizeof(committed));
    memset(fake_flash_erases, 0, sizeof(fake_flash_erases));
    fake_flash_violations = 0;
    store_init();
}


static void test_empty_and_basic(void)
{
    Expected settings;
    Expected zone;
    uint8_t small[4];
    uint16_t length;

    printf("Empty, then two records, read back after a restart\n");
    fresh(0xFFFFU);
    CHECK(!store_status()->active);
    CHECK(!store_read(1, small, sizeof(small), &length));
    CHECK(store_write_erases(10));

    make_record(&settings, 42);
    make_record(&zone, 124);
    CHECK(store_write(1, settings.data, settings.length) == STORE_OK);
    CHECK(store_status()->active && store_status()->generation == 1 &&
          store_status()->page_changes == 1);
    CHECK(store_write(2, zone.data, zone.length) == STORE_OK);
    CHECK(store_status()->appends == 1);
    committed[1] = settings;
    committed[2] = zone;
    store_init();
    CHECK(all_read_as_committed());
    CHECK(!store_read(1, small, sizeof(small), &length)); // too long for the buffer
    CHECK(!store_read(3, small, sizeof(small), &length));
    CHECK(store_write(0, small, 1) == STORE_BAD_ARGUMENT);
    CHECK(store_write(STORE_TYPES, small, 1) == STORE_BAD_ARGUMENT);
    CHECK(store_write(1, settings.data, STORE_RECORD_MAX_BYTES + 1U) == STORE_BAD_ARGUMENT);
    CHECK(fake_flash_violations == 0);

    // Odd and zero lengths.
    make_record(&settings, 1);
    CHECK(store_write(1, settings.data, 1) == STORE_OK);
    committed[1] = settings;
    make_record(&settings, 0);
    CHECK(store_write(3, settings.data, 0) == STORE_OK);
    committed[3] = settings;
    store_init();
    CHECK(all_read_as_committed());
}


static void test_wear(void)
{
    Expected zone;
    int index;
    uint32_t writes = 2000;

    printf("Wear: %lu settings writes with a time zone record kept\n", (unsigned long)writes);
    fresh(0xFFFFU);
    make_record(&zone, 124);
    CHECK(store_write(2, zone.data, zone.length) == STORE_OK);
    committed[2] = zone;
    for (index = 0; index < (int)writes; index++)
    {
        Expected settings;

        make_record(&settings, 42);
        if (store_write(1, settings.data, settings.length) != STORE_OK)
        {
            CHECK(0);
            break;
        }
        committed[1] = settings;
        if (index % 97 == 0)
        {
            store_init(); // a restart now and then
            CHECK(all_read_as_committed());
        }
    }
    store_init();
    CHECK(all_read_as_committed());
    CHECK(fake_flash_violations == 0);
    printf("  erases: page 0 %lu, page 1 %lu (%.1f settings writes per erase), generation %u\n",
           (unsigned long)fake_flash_erases[0], (unsigned long)fake_flash_erases[1],
           (double)writes / (double)(fake_flash_erases[0] + fake_flash_erases[1]),
           store_status()->generation);
    CHECK(fake_flash_erases[0] + fake_flash_erases[1] < writes / 30U);
}


/*
 * Power cut at every operation of many writes: after the restart, every
 * type reads as it was committed, except the one being written, which
 * reads as either its old or its new value. And the store carries on.
 */
static void test_power_cuts(void)
{
    int round;
    static int cuts;
    static int took_new;
    int rounds = 20000;

    printf("Power cut at a random point of %d writes\n", rounds);
    fresh(0xFFFFU);
    fake_flash_seed(99U);
    for (round = 0; round < rounds; round++)
    {
        uint8_t type = (uint8_t)(1U + next_random() % (STORE_TYPES - 1U));
        Expected record;
        volatile int cut = 0;

        make_record(&record, (uint16_t)(next_random() % (STORE_RECORD_MAX_BYTES + 1U)));
        fake_flash_cut_after = (long)(next_random() % 80U);
        if (setjmp(fake_flash_reset) == 0)
        {
            StoreResult result = store_write(type, record.data, record.length);
            CHECK(result == STORE_OK);
        }
        else
        {
            cut = 1;
        }
        fake_flash_cut_after = -1;
        store_init(); // the restart
        if (cut)
        {
            cuts++;
            if (reads_as(type, &record))
            {
                committed[type] = record;
                took_new++;
            }
        }
        else
        {
            committed[type] = record;
        }
        if (!all_read_as_committed())
        {
            CHECK(0);
            printf("  round %d, type %u, cut %d\n", round, type, cut);
            break;
        }
    }
    CHECK(cuts > rounds / 4);
    CHECK(fake_flash_violations == 0); // nothing written over, torn records included
    printf("  %d cut short (%d of those had already written the new record), %lu erases, "
           "generation %u, %u torn records passed over at the last start\n",
           cuts, took_new, (unsigned long)(fake_flash_erases[0] + fake_flash_erases[1]),
           store_status()->generation, store_status()->torn);
}


static void test_garbage(void)
{
    Expected settings;
    Expected zone;

    printf("Pages holding something else\n");
    // Never erased, garbage everywhere: nothing read, the first write
    // erases.
    fresh(0x1234U);
    CHECK(!store_status()->active);
    make_record(&settings, 42);
    CHECK(store_write(1, settings.data, settings.length) == STORE_OK);
    committed[1] = settings;
    store_init();
    CHECK(all_read_as_committed());

    // Junk after the last record: the page is treated as full, and the next
    // write moves to the other page with everything intact.
    make_record(&zone, 124);
    CHECK(store_write(2, zone.data, zone.length) == STORE_OK);
    committed[2] = zone;
    fake_flash_words[store_status()->page][store_status()->used_words] = 0x0000U;
    store_init();
    CHECK(store_status()->unreadable && store_status()->full);
    CHECK(all_read_as_committed());
    CHECK(store_write_erases(2));
    make_record(&settings, 42);
    CHECK(store_write(1, settings.data, settings.length) == STORE_OK);
    committed[1] = settings;
    store_init();
    CHECK(!store_status()->unreadable && all_read_as_committed());

    // Something programmed a little way into the free space: never written
    // over (that would program those words twice); the write moves to the
    // other page instead.
    {
        uint8_t page = store_status()->page;

        fake_flash_words[page][store_status()->used_words + 6U] = 0x1234U;
        store_init();
        CHECK(!store_status()->unreadable);
        make_record(&zone, 124);
        CHECK(store_write(2, zone.data, zone.length) == STORE_OK);
        committed[2] = zone;
        CHECK(store_status()->page != page && fake_flash_violations == 0);
        store_init();
        CHECK(all_read_as_committed());
    }

    // A record whose data has decayed: passed over, the one before it
    // stands.
    {
        Expected newer;
        uint16_t at = store_status()->used_words;

        make_record(&newer, 42);
        CHECK(store_write(1, newer.data, newer.length) == STORE_OK);
        fake_flash_words[store_status()->page][at + 3U] ^= 0x0100U;
        store_init();
        CHECK(store_status()->torn == 1);
        CHECK(all_read_as_committed()); // the older settings record
    }
    CHECK(fake_flash_violations == 0);
}


static void test_generation_wrap(void)
{
    Expected settings;
    uint16_t generation;

    printf("Generation wraps\n");
    fresh(0xFFFFU);
    make_record(&settings, 42);
    CHECK(store_write(1, settings.data, settings.length) == STORE_OK);
    committed[1] = settings;
    // Rewrite the header in place as generation 0xFFFF (its CRC too).
    {
        uint8_t page = store_status()->page;
        uint16_t words[3] = {STORE_MAGIC, STORE_FORMAT, 0xFFFFU};
        uint8_t bytes[6];
        uint16_t crc;
        int index;

        for (index = 0; index < 3; index++)
        {
            bytes[2 * index] = (uint8_t)words[index];
            bytes[2 * index + 1] = (uint8_t)(words[index] >> 8);
        }
        crc = sml_crc(SML_CRC_INIT, bytes, 6U);
        fake_flash_words[page][2] = 0xFFFFU;
        fake_flash_words[page][3] = crc;
    }
    store_init();
    CHECK(store_status()->active && store_status()->generation == 0xFFFFU);
    // Fill until the page changes: generation 0 must win over 0xFFFF.
    generation = store_status()->generation;
    while (store_status()->generation == generation)
    {
        make_record(&settings, 100);
        CHECK(store_write(1, settings.data, settings.length) == STORE_OK);
        committed[1] = settings;
    }
    CHECK(store_status()->generation == 0U);
    store_init();
    CHECK(store_status()->generation == 0U && all_read_as_committed());
}


int main(void)
{
    test_empty_and_basic();
    test_wear();
    test_power_cuts();
    test_garbage();
    test_generation_wrap();
    {
        static const char *const names[STORE_TYPES] = {NULL, "one", "two", "three"};
        store_print(names, false);
        store_print(names, true);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
