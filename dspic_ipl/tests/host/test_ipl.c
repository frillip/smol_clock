/*
 * The IPL's selection and CRC-32 (ipl_select.c, ipl_crc32.c), against a
 * model of program memory: every combination of the SPL's state, the
 * application's, the trial count and the mailbox, checked against the
 * specification's rules written out again here (4.3), then the sequences
 * that matter (an SPL that never comes up, a power cut, a corrupt image),
 * torn record writes, and the CRC-32 against common/boot_crc.c.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/boot_crc.h"
#include "../../../common/boot_layout.h"
#include "../../../common/sml_proto.h"
#include "../../ipl_select.h"

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

// ---- Program memory -----------------------------------------------------------

#define FLASH_WORDS (0x02B000UL / 2)
#define ERASED      0xFFFFFFUL

static uint32_t flash[FLASH_WORDS];
static unsigned long reads;

uint32_t ipl_flash_read(uint32_t address)
{
    reads++;
    if ((address & 1UL) != 0UL || address / 2 >= FLASH_WORDS)
    {
        printf("  FAIL: read at 0x%06lX\n", (unsigned long)address);
        failures++;
        return ERASED;
    }
    return flash[address / 2];
}


uint32_t ipl_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions);

static void erase_all(void)
{
    for (uint32_t i = 0; i < FLASH_WORDS; i++)
        flash[i] = ERASED;
}


static void put(uint32_t address, uint32_t word)
{
    flash[address / 2] = word & 0xFFFFFFUL;
}


// A record word, as written: 16 bits, upper byte 0xFF.
static void put_word(uint32_t record, int index, uint16_t value)
{
    put(record + 2U * (uint32_t)index, 0xFF0000U | value);
}


static uint16_t get_word(uint32_t record, int index)
{
    return (uint16_t)flash[(record + 2U * (uint32_t)index) / 2];
}


static uint32_t packed_crc(uint32_t start, uint32_t length)
{
    uint32_t crc = BOOT_CRC32_INIT;
    for (uint32_t i = 0; i < length; i++)
    {
        uint32_t word = flash[(start + 2 * i) / 2];
        uint8_t bytes[3] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)};
        crc = boot_crc32_update(crc, bytes, 3);
    }
    return ~crc;
}


// Code: words from its own generator, so the tests' rand() runs on.
static void put_code(uint32_t start, uint32_t length, unsigned seed)
{
    uint32_t state = seed * 2654435761U + 1U;
    for (uint32_t i = 0; i < length; i++)
    {
        state = state * 1664525U + 1013904223U;
        put(start + 2 * i, state >> 8);
    }
}


// ---- Records ----------------------------------------------------------------

typedef enum
{
    SPL_BLANK,        // nothing there
    SPL_PARTIAL,      // code and record words, no commit
    SPL_TORN_COMMIT,  // commit partly programmed
    SPL_BAD_CRC,      // record_crc wrong
    SPL_CORRUPT,      // committed, the code changed since
    SPL_COMMITTED,    // committed, unconfirmed
    SPL_CONFIRMED,    // committed and confirmed
    SPL_TORN_CONFIRM, // confirm partly programmed: unconfirmed
    SPL_REJECTED,     // committed, unconfirmed, rejected
    SPL_TORN_REJECT,  // reject partly programmed: still rejected
    SPL_BAD_LENGTH,   // odd length
    SPL_KINDS
} SplKind;

static const char *const spl_names[] = {"blank",    "partial",     "torn commit", "bad crc",
                                        "corrupt",  "committed",   "confirmed",   "torn confirm",
                                        "rejected", "torn reject", "bad length"};

typedef enum
{
    APP_BLANK,
    APP_PARTIAL,
    APP_TORN_COMMIT,
    APP_CORRUPT,
    APP_WRONG_START,
    APP_VALID,
    APP_KINDS
} AppKind;

static const char *const app_names[] = {"blank",   "partial",     "torn commit",
                                        "corrupt", "wrong start", "valid"};

static void write_record_header(uint32_t record, uint16_t magic, const uint16_t *fields, int count)
{
    uint16_t words[RECORD_CRC_WORDS] = {0};
    words[RECORD_MAGIC] = magic;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    memcpy(&words[2], fields, sizeof(uint16_t) * (size_t)count);
    for (int i = 0; i < RECORD_CRC_WORDS; i++)
        put_word(record, i, words[i]);
    put_word(record, RECORD_CRC, boot_record_crc(words));
    put_word(record, RECORD_SPARE, 0xFFFF);
}


static void write_spl(SplKind kind, uint32_t length)
{
    if (kind == SPL_BLANK)
        return;
    put_code(SPL_START, length, 7);
    uint32_t crc = packed_crc(SPL_START, length);
    if (kind == SPL_BAD_LENGTH)
        length++;
    uint16_t fields[9] = {0x0001,
                          0x0203,
                          (uint16_t)(length >> 16),
                          (uint16_t)length,
                          (uint16_t)(crc >> 16),
                          (uint16_t)crc,
                          0x6A08,
                          0x1F00,
                          SML_BOOT_PROTOCOL};
    write_record_header(SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, fields, 9);
    if (kind == SPL_BAD_CRC)
        put_word(SPL_RECORD_ADDRESS, RECORD_CRC,
                 (uint16_t)(get_word(SPL_RECORD_ADDRESS, RECORD_CRC) ^ 1));
    if (kind == SPL_PARTIAL)
        return;
    if (kind == SPL_TORN_COMMIT)
    {
        put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
        put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1 | 0x0100);
        return;
    }
    put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    if (kind == SPL_CORRUPT)
        put(SPL_START + 2 * (length / 2), flash[(SPL_START + 2 * (length / 2)) / 2] ^ 0x000100);
    if (kind == SPL_CONFIRMED)
    {
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
    }
    if (kind == SPL_TORN_CONFIRM)
    {
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0 | 0x0003);
    }
    if (kind == SPL_REJECTED)
    {
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, SPL_REJECT_VALUE);
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT + 1, SPL_REJECT_VALUE);
    }
    if (kind == SPL_TORN_REJECT)
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT + 1, 0xFFFE);
}


static void write_app(AppKind kind, uint32_t length)
{
    if (kind == APP_BLANK)
        return;
    put_code(APP_START, length, 11);
    uint32_t crc = packed_crc(APP_START, length);
    uint32_t start = kind == APP_WRONG_START ? APP_START + 0x80 : APP_START;
    uint16_t fields[11] = {0x0004,
                           0x0000,
                           (uint16_t)(start >> 16),
                           (uint16_t)start,
                           (uint16_t)(length >> 16),
                           (uint16_t)length,
                           (uint16_t)(crc >> 16),
                           (uint16_t)crc,
                           0x6A08,
                           0x1F00,
                           0x0205};
    write_record_header(APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, fields, 11);
    if (kind == APP_PARTIAL)
        return;
    put_word(APP_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(APP_RECORD_ADDRESS, RECORD_COMMIT + 1,
             kind == APP_TORN_COMMIT ? (uint16_t)(RECORD_COMMIT_1 | 0x8000) : RECORD_COMMIT_1);
    if (kind == APP_CORRUPT)
        put(APP_START + 2 * (length - 2), flash[(APP_START + 2 * (length - 2)) / 2] ^ 1);
}


// ---- The rules, written out again ---------------------------------------------

typedef struct
{
    bool committed, intact, confirmed, rejected;
} Expect;

static Expect spl_expect(SplKind kind)
{
    Expect e = {false, false, false, false};
    e.committed = kind == SPL_CORRUPT || kind == SPL_COMMITTED || kind == SPL_CONFIRMED ||
                  kind == SPL_TORN_CONFIRM || kind == SPL_REJECTED || kind == SPL_TORN_REJECT;
    e.intact = e.committed && kind != SPL_CORRUPT;
    e.confirmed = kind == SPL_CONFIRMED;
    e.rejected = kind == SPL_REJECTED || kind == SPL_TORN_REJECT;
    return e;
}


static void check_choice(SplKind spl_kind, AppKind app_kind, uint16_t trials_before, bool hung,
                         const BootMailbox *after, IplChoice choice)
{
    Expect spl = spl_expect(spl_kind);
    bool app = app_kind == APP_VALID;
    IplStart start;
    uint16_t boot = hung ? BOOT_PATH_SPL_HUNG : 0;
    bool trial = false;
    uint16_t trials = trials_before;

    // A confirmed SPL's trials: its hangs in a row.
    if (spl.committed && spl.confirmed)
        trials = hung ? (uint16_t)(trials == 0xFFFF ? trials : trials + 1) : 0;
    if (spl.intact && !spl.rejected && trials < SPL_TRIAL_LIMIT)
    {
        start = IPL_START_SPL;
        trial = !spl.confirmed;
    }
    else if (app)
    {
        start = IPL_START_APP;
        boot |= BOOT_PATH_APP_DIRECT | (spl.intact ? BOOT_PATH_SPL_GIVEN_UP : BOOT_PATH_NO_SPL);
    }
    else if (spl.intact)
    {
        start = IPL_START_SPL;
        boot |= BOOT_PATH_LAST_RESORT;
        trial = !spl.confirmed;
    }
    else
        start = IPL_START_NONE;
    if (trial)
        boot |= BOOT_PATH_UNCONFIRMED;

    bool ok =
        choice.start == start && after->boot == boot &&
        choice.watchdog == (start == IPL_START_SPL) &&
        after->trials == (trial && trials != 0xFFFF ? trials + 1 : trials) &&
        after->running == (start == IPL_START_APP ? MAILBOX_RUNNING_VALUE : 0);
    if (!ok)
        printf("  SPL %s, application %s, trials %u%s: start %d boot 0x%02X trials %u watchdog %d "
               "(expected %d 0x%02X)\n",
               spl_names[spl_kind], app_names[app_kind], trials_before, hung ? ", hung" : "",
               choice.start, after->boot,
               after->trials, choice.watchdog, start, boot);
    CHECK(ok);
    // Never anything whose image does not check.
    if (choice.start == IPL_START_SPL)
        CHECK(spl.intact);
    if (choice.start == IPL_START_APP)
        CHECK(app);
    // Nothing started only when nothing could be.
    CHECK((choice.start == IPL_START_NONE) == (!spl.intact && !app));
}


static void test_matrix(void)
{
    static const uint16_t trial_counts[] = {0, 1, 2, 3, 4, 200, 0xFFFF};
    int cases = 0;

    // The reset: a software one, the watchdog while the application ran, and
    // the watchdog while it did not (the SPL hung).
    static const uint16_t causes[] = {SML_RESET_SWR, SML_RESET_WDTO, SML_RESET_WDTO};
    static const uint16_t running[] = {0, MAILBOX_RUNNING_VALUE, 0};

    for (int s = 0; s < SPL_KINDS; s++)
        for (int a = 0; a < APP_KINDS; a++)
            for (size_t t = 0; t < sizeof trial_counts / sizeof trial_counts[0]; t++)
                for (int c = 0; c < 3; c++)
                {
                    BootMailbox mailbox;
                    IplChoice choice;

                    erase_all();
                    write_spl((SplKind)s, 1700);
                    write_app((AppKind)a, 4000);
                    memset(&mailbox, 0, sizeof mailbox);
                    mailbox.magic = MAILBOX_MAGIC_VALUE;
                    mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
                    mailbox.trials = trial_counts[t];
                    mailbox.attempts = 2;
                    mailbox.request = MAILBOX_REQUEST_STAY;
                    mailbox.running = running[c];
                    ipl_mailbox_prepare(&mailbox, causes[c]);
                    choice = ipl_select(&mailbox);
                    check_choice((SplKind)s, (AppKind)a, trial_counts[t], c == 2, &mailbox, choice);
                    // The selection leaves the application's and the SPL's words alone.
                    CHECK(mailbox.attempts == 2 && mailbox.request == MAILBOX_REQUEST_STAY);
                    cases++;
                }
    printf("  %d combinations of SPL, application, trials and reset\n", cases);
}


static void test_mailbox(void)
{
    BootMailbox mailbox;
    static const uint16_t power[] = {SML_RESET_POR, SML_RESET_BOR,
                                     SML_RESET_POR | SML_RESET_BOR | SML_RESET_EXTR};

    // Power-on or brown-out: started again, whatever it held.
    for (size_t i = 0; i < sizeof power / sizeof power[0]; i++)
    {
        memset(&mailbox, 0x5A, sizeof mailbox);
        mailbox.magic = MAILBOX_MAGIC_VALUE;
        mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
        ipl_mailbox_prepare(&mailbox, power[i]);
        CHECK(mailbox.magic == MAILBOX_MAGIC_VALUE &&
              mailbox.magic_inverse == (uint16_t)~MAILBOX_MAGIC_VALUE);
        CHECK(mailbox.request == 0 && mailbox.attempts == 0 && mailbox.trials == 0);
        CHECK(mailbox.running == 0 && mailbox.boot == 0 && mailbox.reset_cause == power[i]);
    }
    // Random RAM (as after a power-on the RCON missed): started again.
    for (int n = 0; n < 1000; n++)
    {
        uint8_t *bytes = (uint8_t *)&mailbox;
        for (size_t i = 0; i < sizeof mailbox; i++)
            bytes[i] = (uint8_t)rand();
        bool valid = mailbox.magic == MAILBOX_MAGIC_VALUE &&
                     mailbox.magic_inverse == (uint16_t)~MAILBOX_MAGIC_VALUE;
        ipl_mailbox_prepare(&mailbox, SML_RESET_WDTO);
        if (!valid)
            CHECK(mailbox.request == 0 && mailbox.attempts == 0 && mailbox.trials == 0);
        CHECK(mailbox.magic == MAILBOX_MAGIC_VALUE && mailbox.running == 0 && mailbox.boot == 0);
        CHECK(mailbox.reset_cause == SML_RESET_WDTO);
    }
    // A valid mailbox over a reset that is not a power-on: kept, but for
    // running and boot. Only the cause bits are recorded.
    memset(&mailbox, 0, sizeof mailbox);
    mailbox.magic = MAILBOX_MAGIC_VALUE;
    mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    mailbox.request = MAILBOX_REQUEST_STAY;
    mailbox.attempts = 2;
    mailbox.trials = 1;
    mailbox.running = MAILBOX_RUNNING_VALUE;
    mailbox.boot = BOOT_PATH_APP_DIRECT;
    ipl_mailbox_prepare(&mailbox, (uint16_t)(SML_RESET_SWR | 0x2008)); // with SWDTEN and SLEEP
    CHECK(mailbox.request == MAILBOX_REQUEST_STAY && mailbox.attempts == 2 && mailbox.trials == 1);
    CHECK(mailbox.running == 0 && mailbox.boot == 0 && mailbox.reset_cause == SML_RESET_SWR);
}


// What the chip would do across resets: the mailbox carried over, a power
// cut starting it again.
static IplChoice reset_into(BootMailbox *mailbox, uint16_t rcon)
{
    ipl_mailbox_prepare(mailbox, rcon);
    return ipl_select(mailbox);
}


static void test_sequences(void)
{
    BootMailbox mailbox;
    IplChoice choice;

    // A new SPL that never comes up: three tries under the watchdog, then the
    // application, which rejects it; then the application at every reset.
    erase_all();
    write_spl(SPL_COMMITTED, 1700);
    write_app(APP_VALID, 4000);
    memset(&mailbox, 0, sizeof mailbox);
    for (int start = 0; start < 3; start++)
    {
        choice = reset_into(&mailbox, start == 0 ? SML_RESET_POR : SML_RESET_WDTO);
        CHECK(choice.start == IPL_START_SPL && choice.watchdog);
        CHECK(mailbox.boot == (BOOT_PATH_UNCONFIRMED | (start != 0 ? BOOT_PATH_SPL_HUNG : 0)) &&
              mailbox.trials == start + 1);
    }
    choice = reset_into(&mailbox, SML_RESET_WDTO);
    CHECK(choice.start == IPL_START_APP && !choice.watchdog);
    CHECK(mailbox.boot == (BOOT_PATH_APP_DIRECT | BOOT_PATH_SPL_GIVEN_UP | BOOT_PATH_SPL_HUNG));
    CHECK(mailbox.running == MAILBOX_RUNNING_VALUE);
    // A power cut before the application rejects it: three more tries.
    choice = reset_into(&mailbox, SML_RESET_POR | SML_RESET_BOR);
    CHECK(choice.start == IPL_START_SPL && mailbox.trials == 1);
    // Rejected: the application, power cut or not.
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, 0);
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT + 1, 0);
    for (int start = 0; start < 5; start++)
    {
        choice = reset_into(&mailbox, start == 0 ? SML_RESET_POR : SML_RESET_SWR);
        CHECK(choice.start == IPL_START_APP);
        CHECK(mailbox.boot == (BOOT_PATH_APP_DIRECT | BOOT_PATH_SPL_GIVEN_UP));
    }

    // A good new SPL: confirmed within its trials, then started for good,
    // under the watchdog, its trials zeroed.
    erase_all();
    write_spl(SPL_COMMITTED, 1700);
    write_app(APP_VALID, 4000);
    memset(&mailbox, 0, sizeof mailbox);
    choice = reset_into(&mailbox, SML_RESET_POR);
    CHECK(choice.start == IPL_START_SPL && choice.watchdog && mailbox.trials == 1);
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
    for (int start = 0; start < 10; start++)
    {
        choice = reset_into(&mailbox, SML_RESET_SWR);
        CHECK(choice.start == IPL_START_SPL && choice.watchdog && mailbox.boot == 0);
        CHECK(mailbox.trials == 0);
    }

    // That confirmed SPL hanging: a watchdog reset while it runs counts, three
    // in a row give the application, which an ordinary reset of its own (or
    // its watchdog) follows with the SPL again. Without the application, the
    // SPL whatever its count.
    choice = reset_into(&mailbox, SML_RESET_POR);
    CHECK(choice.start == IPL_START_SPL && mailbox.trials == 0);
    for (int hang = 1; hang <= 3; hang++)
    {
        choice = reset_into(&mailbox, SML_RESET_WDTO);
        CHECK((mailbox.boot & BOOT_PATH_SPL_HUNG) != 0 && mailbox.trials == hang);
        CHECK(choice.start == (hang < 3 ? IPL_START_SPL : IPL_START_APP));
    }
    CHECK(mailbox.boot == (BOOT_PATH_APP_DIRECT | BOOT_PATH_SPL_GIVEN_UP | BOOT_PATH_SPL_HUNG));
    CHECK(mailbox.running == MAILBOX_RUNNING_VALUE && !choice.watchdog);
    choice = reset_into(&mailbox, SML_RESET_WDTO); // the application's own watchdog
    CHECK(choice.start == IPL_START_SPL && mailbox.boot == 0 && mailbox.trials == 0);
    choice = reset_into(&mailbox, SML_RESET_WDTO);
    CHECK(choice.start == IPL_START_SPL && mailbox.trials == 1);
    choice = reset_into(&mailbox, SML_RESET_SWR); // not a hang: the count starts again
    CHECK(choice.start == IPL_START_SPL && mailbox.trials == 0 && mailbox.boot == 0);
    for (int start = 0; start < 3; start++)
        choice = reset_into(&mailbox, SML_RESET_WDTO);
    CHECK(choice.start == IPL_START_APP);
    // The application erased meanwhile (an update through this SPL): the
    // SPL, as a last resort, however often it hangs.
    erase_all();
    write_spl(SPL_CONFIRMED, 1700);
    for (int start = 0; start < 7; start++)
    {
        // The first: the application's own watchdog (it was running).
        choice = reset_into(&mailbox, SML_RESET_WDTO);
        CHECK(choice.start == IPL_START_SPL && choice.watchdog);
        CHECK(mailbox.boot == (start == 0   ? 0
                               : start < 3 ? BOOT_PATH_SPL_HUNG
                                            : (BOOT_PATH_SPL_HUNG | BOOT_PATH_LAST_RESORT)));
    }

    // An SPL update cut short (record gone, code half written): the
    // application, which writes the SPL again.
    erase_all();
    write_app(APP_VALID, 4000);
    put_code(SPL_START, 800, 3);
    memset(&mailbox, 0, sizeof mailbox);
    choice = reset_into(&mailbox, SML_RESET_POR);
    CHECK(choice.start == IPL_START_APP &&
          mailbox.boot == (BOOT_PATH_APP_DIRECT | BOOT_PATH_NO_SPL));

    // An application update cut short: the SPL, confirmed, waits for the ESP.
    erase_all();
    write_spl(SPL_CONFIRMED, 1700);
    put_code(APP_START, 3000, 5);
    choice = reset_into(&mailbox, SML_RESET_POR);
    CHECK(choice.start == IPL_START_SPL && mailbox.boot == 0 && choice.watchdog);

    // Both gone: nothing, and no read outside program memory on the way.
    erase_all();
    choice = reset_into(&mailbox, SML_RESET_POR);
    CHECK(choice.start == IPL_START_NONE && mailbox.boot == 0 && mailbox.running == 0);
}


// A record word partly programmed: some of the bits meant to be 0 still 1.
// Commit and confirm count only when exact; reject counts with any bit.
static void test_torn_words(void)
{
    BootMailbox mailbox;
    int counted = 0;

    for (int n = 0; n < 3000; n++)
    {
        erase_all();
        write_spl(SPL_PARTIAL, 64);
        write_app(APP_VALID, 64);
        int which = n % 3;
        uint16_t target0 = which == 0 ? RECORD_COMMIT_0 : which == 1 ? SPL_CONFIRM_0 : 0x0000;
        uint16_t target1 = which == 0 ? RECORD_COMMIT_1 : which == 1 ? SPL_CONFIRM_1 : 0x0000;
        // Each bit meant to be 0 programmed or not, at random; 1 in 8 complete.
        uint16_t left0 = (rand() % 8) ? (uint16_t)(rand() & ~target0) : 0;
        uint16_t left1 = (rand() % 8) ? (uint16_t)(rand() & ~target1) : 0;
        bool exact = left0 == 0 && left1 == 0;
        int index = which == 0   ? RECORD_COMMIT
                    : which == 1 ? SPL_RECORD_CONFIRM
                                 : SPL_RECORD_REJECT;

        if (which != 0)
        {
            put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
            put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
        }
        put_word(SPL_RECORD_ADDRESS, index, (uint16_t)(target0 | left0));
        put_word(SPL_RECORD_ADDRESS, index + 1, (uint16_t)(target1 | left1));
        memset(&mailbox, 0, sizeof mailbox);
        IplChoice choice = reset_into(&mailbox, SML_RESET_POR);
        if (which == 0)
            CHECK((choice.start == IPL_START_SPL) == exact);
        if (which == 1)
            CHECK(choice.start == IPL_START_SPL && choice.watchdog &&
                  ((mailbox.boot & BOOT_PATH_UNCONFIRMED) != 0) == !exact);
        if (which == 2)
        {
            bool programmed =
                (uint16_t)(target0 | left0) != 0xFFFF || (uint16_t)(target1 | left1) != 0xFFFF;
            CHECK((choice.start == IPL_START_APP) == programmed);
        }
        counted += exact;
    }
    printf("  3000 torn commit, confirm and reject words (%d complete)\n", counted);
}


static void test_crc(void)
{
    // The test vector, packed as the images are.
    erase_all();
    put(0x2000, 0x333231);
    put(0x2002, 0x363534);
    put(0x2004, 0x393837);
    CHECK(~ipl_crc32_flash(BOOT_CRC32_INIT, 0x2000, 3) == 0xCBF43926UL);
    // Against boot_crc.c, over random flash, across the 0x10000 boundary,
    // and continued in parts.
    for (int n = 0; n < 50; n++)
    {
        uint32_t start = 0xF000U + 2U * (uint32_t)(rand() % 0x1000);
        uint32_t length = (uint32_t)(rand() % 6000);
        uint32_t split = length ? (uint32_t)(rand() % (int)length) : 0;
        put_code(start, length, (unsigned)n);
        uint32_t whole = ~ipl_crc32_flash(BOOT_CRC32_INIT, start, length);
        CHECK(whole == packed_crc(start, length));
        CHECK(~ipl_crc32_flash(ipl_crc32_flash(BOOT_CRC32_INIT, start, split), start + 2 * split,
                               length - split) == whole);
    }
    // The selection's record CRC, bitwise, against boot_record_crc().
    for (int n = 0; n < 200; n++)
    {
        BootMailbox mailbox;
        erase_all();
        write_spl(SPL_CONFIRMED, (uint32_t)(2 * (1 + rand() % 1504)));
        memset(&mailbox, 0, sizeof mailbox);
        CHECK(reset_into(&mailbox, SML_RESET_POR).start == IPL_START_SPL);
    }
}


// The cost: what the IPL reads for the normal start, which sets its time.
static void test_reads(void)
{
    BootMailbox mailbox;

    erase_all();
    write_spl(SPL_CONFIRMED, 1700);
    write_app(APP_VALID, 57600);
    memset(&mailbox, 0, sizeof mailbox);
    reads = 0;
    reset_into(&mailbox, SML_RESET_SWR);
    // The SPL's record (once or twice per word) and its code, not the application.
    CHECK(reads < 1700 + 200);
    printf("  normal start: %lu flash reads\n", reads);
}


int main(void)
{
    srand(1);
    test_mailbox();
    test_matrix();
    test_sequences();
    test_torn_words();
    test_crc();
    test_reads();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
