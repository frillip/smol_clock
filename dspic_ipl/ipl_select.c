#include "ipl_select.h"

#include "ipl_flash.h"

// An image's state, from its record, and from its CRC-32 once computed.
typedef struct
{
    bool committed; // the record's header, record_crc and commit right
    bool intact;    // ... and the image's CRC-32 matches
    bool confirmed; // SPL: confirm exact
    bool rejected;  // SPL: any bit of reject programmed
} Image;

static uint16_t record_word(uint32_t record, uint8_t index)
{
    return (uint16_t)ipl_flash_read(record + 2U * (uint32_t)index);
}


static uint32_t record_long(uint32_t record, uint8_t index)
{
    return (uint32_t)record_word(record, index) << 16 | record_word(record, (uint8_t)(index + 1U));
}


// CRC-16/IBM-3740 over words 0-13, each big-endian: sml_crc's algorithm,
// bitwise here, as the IPL has room and no tables.
static uint16_t record_crc(uint32_t record)
{
    uint16_t crc = 0xFFFFU;
    uint8_t index;
    uint8_t bit;

    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        crc ^= record_word(record, index);
        for (bit = 0; bit < 16U; bit++)
        {
            crc = (crc & 0x8000U) != 0U ? (uint16_t)(crc << 1) ^ 0x1021U : (uint16_t)(crc << 1);
        }
    }
    return crc;
}


static bool double_word_is(uint32_t record, uint8_t index, uint16_t first, uint16_t second)
{
    return record_word(record, index) == first &&
           record_word(record, (uint8_t)(index + 1U)) == second;
}


// The common part of a record: magic, format, record_crc, commit, and a
// length that is even, not zero and at most max.
static bool record_committed(uint32_t record, uint16_t magic, uint8_t length_index, uint32_t max)
{
    uint32_t length = record_long(record, length_index);

    return record_word(record, RECORD_MAGIC) == magic &&
           record_word(record, RECORD_FORMAT) == RECORD_FORMAT_1 && length != 0UL &&
           (length & 1UL) == 0UL && length <= max &&
           record_word(record, RECORD_CRC) == record_crc(record) &&
           double_word_is(record, RECORD_COMMIT, RECORD_COMMIT_0, RECORD_COMMIT_1);
}


static bool image_matches(uint32_t start, uint32_t record, uint8_t length_index, uint8_t crc_index)
{
    uint32_t crc = ~ipl_crc32_flash(0xFFFFFFFFUL, start, record_long(record, length_index));

    return crc == record_long(record, crc_index);
}


static Image spl_state(void)
{
    Image spl = {false, false, false, false};

    spl.committed = record_committed(SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, SPL_RECORD_LENGTH,
                                     SPL_CODE_MAX);
    if (spl.committed)
    {
        spl.confirmed =
            double_word_is(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0, SPL_CONFIRM_1);
        spl.rejected = !double_word_is(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, 0xFFFFU, 0xFFFFU);
    }
    return spl;
}


static bool spl_intact(Image *spl)
{
    if (spl->committed && !spl->intact)
    {
        spl->intact =
            image_matches(SPL_START, SPL_RECORD_ADDRESS, SPL_RECORD_LENGTH, SPL_RECORD_CRC32);
        spl->committed = spl->intact; // a corrupt image is no SPL: not checked again
    }
    return spl->intact;
}


static bool app_valid(void)
{
    return record_committed(APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, APP_RECORD_LENGTH,
                            APP_MAX) &&
           record_long(APP_RECORD_ADDRESS, APP_RECORD_START) == APP_START &&
           image_matches(APP_START, APP_RECORD_ADDRESS, APP_RECORD_LENGTH, APP_RECORD_CRC32);
}


void ipl_mailbox_prepare(BootMailbox *mailbox, uint16_t rcon)
{
    bool kept = (rcon & BOOT_RESET_POWER) == 0U && mailbox->magic == MAILBOX_MAGIC_VALUE &&
                mailbox->magic_inverse == (uint16_t)~MAILBOX_MAGIC_VALUE;

    // The watchdog, while the application was not running: the SPL hung
    // (the IPL itself never runs under it).
    mailbox->boot = kept && (rcon & BOOT_RESET_WDTO) != 0U &&
                            mailbox->running != MAILBOX_RUNNING_VALUE
                        ? BOOT_PATH_SPL_HUNG
                        : 0U;
    if (!kept)
    {
        mailbox->magic = MAILBOX_MAGIC_VALUE;
        mailbox->magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
        mailbox->request = 0;
        mailbox->attempts = 0;
        mailbox->trials = 0;
    }
    mailbox->running = 0;
    mailbox->reset_cause = (uint16_t)(rcon & BOOT_RESET_CAUSES);
}


static void count_trial(BootMailbox *mailbox)
{
    if (mailbox->trials != 0xFFFFU)
    {
        mailbox->trials++;
    }
}


// An SPL start, always under the watchdog: an unconfirmed one uses a trial.
static IplChoice start_spl(BootMailbox *mailbox, const Image *spl)
{
    IplChoice choice = {IPL_START_SPL, true};

    if (!spl->confirmed)
    {
        mailbox->boot |= BOOT_PATH_UNCONFIRMED;
        count_trial(mailbox);
    }
    return choice;
}


IplChoice ipl_select(BootMailbox *mailbox)
{
    IplChoice none = {IPL_START_NONE, false};
    Image spl = spl_state();

    // A confirmed SPL's trials count its hangs in a row instead: zeroed by
    // any other reset, so a hang that recurs gives way to the application,
    // and the SPL is tried again at its next reset.
    if (spl.committed && spl.confirmed)
    {
        if ((mailbox->boot & BOOT_PATH_SPL_HUNG) != 0U)
        {
            count_trial(mailbox);
        }
        else
        {
            mailbox->trials = 0;
        }
    }
    // 1: the SPL, in the normal way.
    if (spl.committed && !spl.rejected && mailbox->trials < SPL_TRIAL_LIMIT && spl_intact(&spl))
    {
        return start_spl(mailbox, &spl);
    }
    // 2: the application, which can install an SPL.
    if (app_valid())
    {
        IplChoice choice = {IPL_START_APP, false};

        mailbox->boot |=
            BOOT_PATH_APP_DIRECT | (spl_intact(&spl) ? BOOT_PATH_SPL_GIVEN_UP : BOOT_PATH_NO_SPL);
        mailbox->running = MAILBOX_RUNNING_VALUE;
        return choice;
    }
    // 3: the SPL, whatever its trials and reject say: nothing else is left.
    if (spl_intact(&spl))
    {
        mailbox->boot |= BOOT_PATH_LAST_RESORT;
        return start_spl(mailbox, &spl);
    }
    return none;
}
