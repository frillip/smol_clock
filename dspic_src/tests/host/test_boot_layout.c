/*
 * The bootloader's shared definitions (common/boot_layout.h, the class 0xF0
 * part of common/sml_proto.h) against the specification
 * (claude/v2plus-bootloader.md, sections 3, 4 and 14), and its checksums
 * (common/boot_crc.c) against zlib's vectors and a bitwise reference.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/boot_crc.h"
#include "../../../common/boot_layout.h"
#include "../../../common/sml.h"

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

// The specification's values, as written there (section 14 and the map).
static void test_values(void)
{
    CHECK(IPL_SERVICES == 0x000200UL);
    CHECK(IPL_INFO_ADDRESS == 0x000780UL);
    CHECK(SPL_START == 0x000800UL);
    CHECK(SPL_CODE_MAX == 3008UL);
    CHECK(SPL_RECORD_ADDRESS == 0x001F80UL);
    CHECK(APP_START == 0x002000UL && APP_END == 0x029000UL);
    CHECK(APP_FORWARD_TABLE == 0x002004UL);
    CHECK(APP_RECORD_ADDRESS == 0x029000UL);
    CHECK(TEST_START == 0x028800UL && TEST_RECORD_ADDRESS == 0x028F80UL);
    CHECK(STORE_START == 0x029800UL && CONFIG_PAGE == 0x02A800UL && CONFIG_WORDS == 0x02AFECUL);
    CHECK(BOOT_CHUNK_INSTRUCTIONS == 64UL && BOOT_CHUNK_BYTES == 192UL);
    CHECK(MAILBOX_ADDRESS == 0x7FF0);
    CHECK(MAILBOX_MAGIC_VALUE == 0x4D42 && MAILBOX_REQUEST_STAY == 0xB007 &&
          MAILBOX_RUNNING_VALUE == 0xA99A);
    CHECK(APP_RECORD_MAGIC_VALUE == 0x534D && SPL_RECORD_MAGIC_VALUE == 0x5350 &&
          IPL_INFO_MAGIC_VALUE == 0x4950);
    CHECK(RECORD_COMMIT_0 == 0xA55A && RECORD_COMMIT_1 == 0x5AA5);
    CHECK(SPL_CONFIRM_0 == 0xC33C && SPL_CONFIRM_1 == 0x3CC3);
    CHECK(SPL_TRIAL_LIMIT == 3 && BOOT_LOOP_LIMIT == 8 && BOOT_HEALTHY_S == 30 && APP_HEALTHY_S == 60 &&
          BOOT_WINDOW_MS == 1000);
    CHECK(SML_BOOT_KEY_ERASE == 0x45524153UL && SML_BOOT_KEY_COMMIT == 0x434F4D4DUL);
    CHECK(SML_ERROR_CRC == 10 && SML_ERROR_NOT_BLANK == 11);
    CHECK(SML_PROTOCOL_MAJOR == 2 && SML_PROTOCOL_MINOR >= 5); // the BOOT class from 2.5
    // The fault record below the mailbox, clear of it.
    CHECK(FAULT_RECORD_ADDRESS == 0x7FD0 && FAULT_RECORD_BYTES == 32 &&
          FAULT_RECORD_ADDRESS + FAULT_RECORD_BYTES == MAILBOX_ADDRESS);
    // The keys spell what they say, big-endian on the wire.
    {
        uint8_t wire[4];
        sml_put_u32(wire, SML_BOOT_KEY_ERASE);
        CHECK(memcmp(wire, "ERAS", 4) == 0);
        sml_put_u32(wire, SML_BOOT_KEY_COMMIT);
        CHECK(memcmp(wire, "COMM", 4) == 0);
    }
}


// The regions tile program memory as the map has it.
static void test_regions(void)
{
    // Every region starts on a page, every record on a row.
    CHECK(SPL_START % BOOT_PAGE_ADDRESSES == 0 && APP_START % BOOT_PAGE_ADDRESSES == 0);
    CHECK(TEST_START % BOOT_PAGE_ADDRESSES == 0 && APP_RECORD_ADDRESS % BOOT_PAGE_ADDRESSES == 0);
    CHECK(SPL_RECORD_ADDRESS % BOOT_ROW_ADDRESSES == 0 &&
          TEST_RECORD_ADDRESS % BOOT_ROW_ADDRESSES == 0);
    CHECK(IPL_INFO_ADDRESS % BOOT_ROW_ADDRESSES == 0);
    CHECK(BOOT_PAGE_ADDRESSES == 2 * BOOT_PAGE_INSTRUCTIONS);
    CHECK(BOOT_CHUNK_ADDRESSES == 2 * BOOT_CHUNK_INSTRUCTIONS);
    CHECK(BOOT_CHUNK_BYTES == 3 * BOOT_CHUNK_INSTRUCTIONS);

    // IPL, SPL, application, record, store, configuration: end to end.
    CHECK(IPL_START == 0 && IPL_END == BOOT_PAGE_ADDRESSES && IPL_END == SPL_START);
    CHECK(IPL_INFO_ADDRESS + BOOT_ROW_ADDRESSES == IPL_END && IPL_CODE_END == IPL_INFO_ADDRESS);
    CHECK(SPL_START + 2 * SPL_CODE_MAX == SPL_RECORD_ADDRESS);
    CHECK(SPL_RECORD_ADDRESS + BOOT_ROW_ADDRESSES == SPL_END);
    CHECK(SPL_END - SPL_START == SPL_PAGES * BOOT_PAGE_ADDRESSES);
    CHECK(SPL_END == APP_START);
    CHECK(APP_END - APP_START == 2 * APP_MAX);
    CHECK(APP_END - APP_START == APP_PAGES * BOOT_PAGE_ADDRESSES);
    CHECK(APP_RECORD_ADDRESS == APP_END);
    CHECK(APP_RECORD_ADDRESS + BOOT_PAGE_ADDRESSES == STORE_START);
    CHECK(STORE_START + 2 * BOOT_PAGE_ADDRESSES == CONFIG_PAGE);
    CHECK(CONFIG_WORDS > CONFIG_PAGE && CONFIG_WORDS < CONFIG_PAGE + BOOT_PAGE_ADDRESSES);
    CHECK(BOOT_VERIFY_LIMIT == STORE_START);

    // The test page: the application region's last, its record in its last row.
    CHECK(TEST_START + BOOT_PAGE_ADDRESSES == APP_END);
    CHECK(TEST_START + 2 * TEST_CODE_MAX == TEST_RECORD_ADDRESS);
    CHECK(TEST_RECORD_ADDRESS + BOOT_ROW_ADDRESSES == APP_END);

    // The forwarding table: IVT entry A (0x0004-0x01FE) to 0x002004 + 2(A - 4),
    // a GOTO each, after the entry GOTO and inside the first 0x400.
    CHECK(APP_FORWARD_TABLE == APP_START + 4);
    CHECK(APP_VECTORS == (0x0200 - 0x0004) / 2);
    CHECK(APP_FORWARD_TABLE + 2 * (0x01FE - 0x0004) + 4 <= APP_START + 0x400);
    CHECK(APP_FORWARD_TABLE + 4 * APP_VECTORS == 0x0023FCUL);

    // Records fit their row: a word per instruction, 2 address units each.
    CHECK(2 * APP_RECORD_WORDS <= BOOT_ROW_ADDRESSES);
    CHECK(2 * SPL_RECORD_WORDS <= BOOT_ROW_ADDRESSES);
    CHECK(2 * IPL_INFO_WORDS <= BOOT_ROW_ADDRESSES);
    CHECK(RECORD_CRC == RECORD_CRC_WORDS && RECORD_COMMIT == RECORD_SPARE + 1);
    CHECK(SPL_RECORD_CONFIRM == RECORD_COMMIT + 2 && SPL_RECORD_REJECT == SPL_RECORD_CONFIRM + 2);
    CHECK(SPL_RECORD_REJECT + 2 == SPL_RECORD_WORDS && RECORD_COMMIT + 2 == APP_RECORD_WORDS);
    // Commit, confirm and reject each a double word: an even word index.
    CHECK(RECORD_COMMIT % 2 == 0 && SPL_RECORD_CONFIRM % 2 == 0 && SPL_RECORD_REJECT % 2 == 0);
    CHECK(APP_RECORD_SML_VERSION < RECORD_CRC && SPL_RECORD_PROTOCOL < RECORD_CRC);
}


static void test_mailbox_and_wire(void)
{
    // Below 0x8000 (no EDS), inside the 32 KB RAM, word-aligned, at the top.
    CHECK(sizeof(BootMailbox) == 2 * MAILBOX_WORDS);
    CHECK(MAILBOX_ADDRESS >= 0x1000 && MAILBOX_ADDRESS + 2 * MAILBOX_WORDS == 0x8000);
    CHECK(MAILBOX_ADDRESS % 2 == 0);
    CHECK(offsetof(BootMailbox, magic) == 2 * MAILBOX_MAGIC);
    CHECK(offsetof(BootMailbox, magic_inverse) == 2 * MAILBOX_MAGIC_INVERSE);
    CHECK(offsetof(BootMailbox, request) == 2 * MAILBOX_REQUEST);
    CHECK(offsetof(BootMailbox, running) == 2 * MAILBOX_RUNNING);
    CHECK(offsetof(BootMailbox, attempts) == 2 * MAILBOX_ATTEMPTS);
    CHECK(offsetof(BootMailbox, boot) == 2 * MAILBOX_BOOT);
    CHECK(offsetof(BootMailbox, trials) == 2 * MAILBOX_TRIALS);
    CHECK(offsetof(BootMailbox, reset_cause) == 2 * MAILBOX_RESET_CAUSE);
    CHECK(BOOT_RESET_CAUSES == SML_RESET_ALL);
    CHECK(BOOT_RESET_POWER == (SML_RESET_POR | SML_RESET_BOR));

    // The mailbox's boot word goes out as BOOT-STATUS boot: the same bits.
    CHECK(BOOT_PATH_APP_DIRECT == SML_BOOT_PATH_APP_DIRECT);
    CHECK(BOOT_PATH_UNCONFIRMED == SML_BOOT_PATH_UNCONFIRMED);
    CHECK(BOOT_PATH_NO_SPL == SML_BOOT_PATH_NO_SPL);
    CHECK(BOOT_PATH_SPL_GIVEN_UP == SML_BOOT_PATH_SPL_GIVEN_UP);
    CHECK(BOOT_PATH_LAST_RESORT == SML_BOOT_PATH_LAST_RESORT);
    CHECK(BOOT_CHUNK_BYTES == SML_BOOT_CHUNK_BYTES);
    CHECK(BOOT_CHUNK_INSTRUCTIONS == SML_BOOT_CHUNK_INSTRUCTIONS);

    // The offsets the specification's tables give.
    CHECK(offsetof(SML_BOOT_STATUS_PAYLOAD, device_id) == 12);
    CHECK(offsetof(SML_BOOT_STATUS_PAYLOAD, attempts) == 44);
    CHECK(offsetof(SML_BOOT_STATUS_PAYLOAD, ipl_version) == 46);
    CHECK(offsetof(SML_BOOT_STATUS_PAYLOAD, spl_state) == 48);
    CHECK(offsetof(SML_BOOT_STATUS_PAYLOAD, spl_crc32) == 52);
    CHECK(offsetof(SML_BOOT_COMMAND_PAYLOAD, region) == 3);
    CHECK(offsetof(SML_BOOT_COMMAND_PAYLOAD, key) == 4);
    CHECK(offsetof(SML_BOOT_COMMAND_PAYLOAD, args) == 8);
    CHECK(offsetof(SML_BOOT_COMMAND_PAYLOAD, args.extra) == 28);
    CHECK(offsetof(SML_BOOT_RESPONSE_PAYLOAD, region) == 7);
    CHECK(offsetof(SML_BOOT_DATA_PAYLOAD, crc32) == 8);
    CHECK(offsetof(SML_BOOT_DATA_PAYLOAD, data) == 12);
    CHECK(offsetof(SML_BOOT_VERIFY_RESPONSE_PAYLOAD, data) == 12);
    // Directions: the PIC answers, the ESP asks.
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_STATUS)->direction == SML_FROM_PIC);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_COMMAND)->direction == SML_FROM_ESP);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_RESPONSE)->direction == SML_FROM_PIC);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_DATA)->direction == SML_FROM_ESP);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_DATA_RESPONSE)->direction == SML_FROM_PIC);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_VERIFY)->direction == SML_FROM_ESP);
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_VERIFY_RESPONSE)->direction == SML_FROM_PIC);
    // Every BOOT message fits a frame.
    CHECK(SML_BOOT_DATA_BYTES <= SML_MAX_PAYLOAD_BYTES);
    CHECK(SML_BOOT_VERIFY_RESPONSE_BYTES <= SML_MAX_PAYLOAD_BYTES);
}


static uint32_t crc32_bitwise(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    while (length--)
    {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++)
            crc = crc & 1U ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    }
    return ~crc;
}


static void test_crc(void)
{
    static uint8_t data[4096];

    // zlib's vectors.
    CHECK(boot_crc32((const uint8_t *)"", 0) == 0x00000000UL);
    CHECK(boot_crc32((const uint8_t *)"a", 1) == 0xE8B7BE43UL);
    CHECK(boot_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926UL);
    CHECK(boot_crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43) ==
          0x414FA339UL);
    // Packed instructions, as the probe checked on the chip: 0x333231 ... LSB first.
    {
        static const uint8_t packed[] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
        CHECK(boot_crc32(packed, sizeof packed) == 0xCBF43926UL);
    }
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)rand();
    for (int n = 0; n < 200; n++)
    {
        uint32_t length = (uint32_t)(rand() % (int)sizeof data);
        uint32_t split = length ? (uint32_t)(rand() % (int)length) : 0;
        uint32_t whole = boot_crc32(data, length);
        uint32_t parts = ~boot_crc32_update(boot_crc32_update(BOOT_CRC32_INIT, data, split),
                                            data + split, length - split);
        CHECK(whole == crc32_bitwise(data, length));
        CHECK(parts == whole);
    }

    // record_crc: the words, big-endian, through sml_crc.
    {
        uint16_t words[RECORD_CRC_WORDS] = {APP_RECORD_MAGIC_VALUE,
                                            RECORD_FORMAT_1,
                                            0x0004,
                                            0x0000,
                                            0x0000,
                                            0x2000,
                                            0x0000,
                                            0xE0E0,
                                            0x1234,
                                            0x5678,
                                            0x6A08,
                                            0x1F00,
                                            0x0205,
                                            0x0000};
        uint8_t bytes[2 * RECORD_CRC_WORDS];

        for (int i = 0; i < RECORD_CRC_WORDS; i++)
        {
            bytes[2 * i] = (uint8_t)(words[i] >> 8);
            bytes[2 * i + 1] = (uint8_t)words[i];
        }
        CHECK(boot_record_crc(words) == sml_crc(SML_CRC_INIT, bytes, sizeof bytes));
        CHECK(boot_record_crc(words) == 0x3748U); // as tools/test_make_image.py has it
    }
}


int main(void)
{
    test_values();
    test_regions();
    test_mailbox_and_wire();
    test_crc();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
