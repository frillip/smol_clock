#include "boot_image.h"

#include <stddef.h>

#include "../../common/boot_crc.h"
#include "../../common/boot_layout.h"
#include "../../common/sml_proto.h"
#include "flash.h"

static uint16_t word(uint32_t base, uint8_t index)
{
    return flash_read_word(base + 2U * (uint32_t)index);
}


static uint32_t long_word(uint32_t base, uint8_t index)
{
    return (uint32_t)word(base, index) << 16 | word(base, (uint8_t)(index + 1U));
}


static bool pair_is(uint32_t base, uint8_t index, uint16_t first, uint16_t second)
{
    return word(base, index) == first && word(base, (uint8_t)(index + 1U)) == second;
}


// The record's header and record_crc right (RECORD), and its commit (COMMITTED).
static uint8_t record_state(uint32_t base, uint16_t magic, uint8_t length_index, uint32_t max)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint32_t length = long_word(base, length_index);
    uint8_t index;

    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        words[index] = word(base, index);
    }
    if (words[RECORD_MAGIC] != magic || words[RECORD_FORMAT] != RECORD_FORMAT_1 || length == 0UL ||
        (length & 1UL) != 0UL || length > max || word(base, RECORD_CRC) != boot_record_crc(words))
    {
        return 0;
    }
    return pair_is(base, RECORD_COMMIT, RECORD_COMMIT_0, RECORD_COMMIT_1)
               ? SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED
               : SML_SPL_STATE_RECORD;
}


void boot_image_ipl(BootIpl *ipl)
{
    ipl->present = word(IPL_INFO_ADDRESS, RECORD_MAGIC) == IPL_INFO_MAGIC_VALUE &&
                   word(IPL_INFO_ADDRESS, RECORD_FORMAT) == RECORD_FORMAT_1;
    ipl->version = ipl->present ? long_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION) : 0UL;
    ipl->services = ipl->present ? word(IPL_INFO_ADDRESS, IPL_INFO_SERVICES) : 0U;
    ipl->build_time = ipl->present ? long_word(IPL_INFO_ADDRESS, IPL_INFO_BUILD_TIME) : 0UL;
}


void boot_image_spl(BootSpl *spl, bool check_crc)
{
    BootIpl ipl;

    spl->state =
        record_state(SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, SPL_RECORD_LENGTH, SPL_CODE_MAX);
    spl->version = 0;
    spl->length = 0;
    spl->crc32 = 0;
    spl->build_time = 0;
    if ((spl->state & SML_SPL_STATE_RECORD) == 0U)
    {
        return;
    }
    spl->version = long_word(SPL_RECORD_ADDRESS, SPL_RECORD_VERSION);
    spl->length = long_word(SPL_RECORD_ADDRESS, SPL_RECORD_LENGTH);
    spl->crc32 = long_word(SPL_RECORD_ADDRESS, SPL_RECORD_CRC32);
    spl->build_time = long_word(SPL_RECORD_ADDRESS, SPL_RECORD_BUILD_TIME);
    if ((spl->state & SML_SPL_STATE_COMMITTED) == 0U)
    {
        return;
    }
    if (pair_is(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0, SPL_CONFIRM_1))
    {
        spl->state |= SML_SPL_STATE_CONFIRMED;
    }
    if (!pair_is(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, 0xFFFFU, 0xFFFFU))
    {
        spl->state |= SML_SPL_STATE_REJECTED;
    }
    boot_image_ipl(&ipl);
    if (check_crc && ipl.present &&
        ~ipl_crc32_flash(BOOT_CRC32_INIT, SPL_START, spl->length) == spl->crc32)
    {
        spl->state |= SML_SPL_STATE_INTACT;
    }
}


void boot_image_app(BootApp *app)
{
    app->committed =
        (record_state(APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, APP_RECORD_LENGTH, APP_MAX) &
         SML_SPL_STATE_COMMITTED) != 0U;
    app->version = app->committed ? long_word(APP_RECORD_ADDRESS, APP_RECORD_VERSION) : 0UL;
    app->length = app->committed ? long_word(APP_RECORD_ADDRESS, APP_RECORD_LENGTH) : 0UL;
    app->crc32 = app->committed ? long_word(APP_RECORD_ADDRESS, APP_RECORD_CRC32) : 0UL;
    app->build_time = app->committed ? long_word(APP_RECORD_ADDRESS, APP_RECORD_BUILD_TIME) : 0UL;
}


bool boot_image_spl_usable(void)
{
    BootSpl spl;

    boot_image_spl(&spl, true);
    return (spl.state & SML_SPL_STATE_INTACT) != 0U && (spl.state & SML_SPL_STATE_REJECTED) == 0U;
}
