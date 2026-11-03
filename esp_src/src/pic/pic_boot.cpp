#include "pic_boot.h"

#include <string.h>

#include "../console/console.h"
#include "pic_link.h"
#include "pic_report.h"
#include "pic_store_flash.h"

extern "C"
{
#include "pic_update.h"
#include "sml/sml.h"
#include "spl_update.h"
}

#define BOOT_STATUS_AGE_MS 3000U
#define COMMAND_TOKEN      0x6000U
// Recovery: a PIC left in its SPL without a valid application gets the
// staged file after this long, at most this many times an ESP start.
#define RECOVER_AFTER_MS 30000U
// The SPL sends BOOT-STATUS every second while idle: this long without one,
// and no update running, it has handed over (or the PIC reset into its
// application) and the link resumes.
#define SPL_SILENT_MS 5000U
#define RECOVER_LIMIT    3U

static PicUpdate update;
static SplUpdate spl;
static PicStoredFile spl_staged; // the SPL file being installed
static bool spl_started;
static bool spl_reinstall; // this one puts back the SPL kept: no other after it
static SplUpdateStep spl_shown;
static uint32_t spl_started_ms;
static uint32_t spl_finished_ms;
static uint8_t spl_quarter;
static PicStoreWriter keep_writer; // big: not on the stack
static Sha256 keep_sha;
static PicStoredFile spl_kept; // where the running SPL went
static PicStoredFile staged; // the file being installed
static bool started;         // an update has run since start
static PicUpdateStep shown;  // the step last reported
static uint32_t started_ms;
static uint32_t finished_ms;
static uint8_t quarter; // of the chunks, last reported
static Stream *command_reply;
static uint16_t command_token = COMMAND_TOKEN;
static uint32_t last_running;   // the application version last heard in STAT-SYS
static uint32_t stranded_ms;    // when the PIC was first seen without an application
static bool stranded;
static uint8_t recoveries;

static void send(void *context, uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                 uint16_t length)
{
    (void)context;
    pic_link_send_wire(class_id, message_id, payload, length);
}


static bool read_image(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    (void)context;
    return pic_store_read_image(&staged, offset, data, length);
}


static const PicUpdatePort port = {send, read_image, NULL};


// The SPL update's port: the staged SPL file, and the running SPL kept in
// the SPL slot the new file is not in.
static bool spl_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    (void)context;
    return pic_store_read_image(&spl_staged, offset, data, length);
}


static bool spl_stored(void *context, uint32_t version, uint32_t crc32)
{
    uint8_t slot;

    (void)context;
    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        PicStoredFile file;

        if (pic_store_get(PIC_FILE_SPL, slot, &file) && file.header.version == version &&
            file.header.crc32 == crc32)
        {
            return true;
        }
    }
    return false;
}


static bool keep_begin(void *context, const PicFileHeader *header)
{
    uint8_t bytes[PIC_FILE_HEADER_BYTES];
    uint8_t slot = spl_staged.slot == 0 ? 1U : 0U;

    (void)context;
    spl_kept.valid = false;
    if (pic_store_begin(&keep_writer, PIC_FILE_SPL, slot, pic_file_bytes(header)) != PIC_STORE_OK)
    {
        return false;
    }
    pic_file_spl_header(header, bytes);
    sha256_start(&keep_sha);
    sha256_update(&keep_sha, bytes, sizeof(bytes));
    return pic_store_write(&keep_writer, bytes, sizeof(bytes)) == PIC_STORE_OK;
}


static bool keep_write(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    sha256_update(&keep_sha, data, length);
    return pic_store_write(&keep_writer, data, length) == PIC_STORE_OK;
}


static bool keep_end(void *context)
{
    uint8_t digest[SHA256_BYTES];
    PicStoreResult result;

    (void)context;
    sha256_finish(&keep_sha, digest);
    result = pic_store_finish(&keep_writer, digest, &spl_kept);
    if (result != PIC_STORE_OK)
    {
        console_printf("SPL update: keeping the running SPL: %s\n", pic_store_result_name(result));
        spl_kept.valid = false;
        return false;
    }
    return true;
}


static const SplUpdatePort spl_port = {send,       spl_read, spl_stored, keep_begin,
                                       keep_write, keep_end, NULL};

static void on_frame(const SmlFrame *frame)
{
    if (started && pic_update_running(&update))
    {
        pic_update_frame(&update, frame->class_id, frame->message_id, frame->payload,
                         frame->length, millis());
    }
    if (spl_started && spl_update_running(&spl))
    {
        spl_update_frame(&spl, frame->class_id, frame->message_id, frame->payload, frame->length,
                         millis());
    }
    // The answer to "pic boot run|hold".
    if (command_reply != NULL && frame->class_id == SML_CLASS_BOOT &&
        frame->message_id == SML_BOOT_RESPONSE && frame->length == SML_BOOT_RESPONSE_BYTES &&
        sml_get_u16(&frame->payload[4]) == command_token)
    {
        uint16_t status = sml_get_u16(&frame->payload[0]);

        command_reply->printf("SPL %s: %s", frame->payload[6] == SML_BOOT_CMD_RUN ? "RUN" : "HOLD",
                              pic_report_result_name(status));
        if (status == SML_RESULT_NACK)
        {
            command_reply->printf(" (%s)", pic_report_error_name(sml_get_u16(&frame->payload[2])));
        }
        command_reply->printf("\n");
        command_reply = NULL;
        if (status == SML_RESULT_ACK && frame->payload[6] == SML_BOOT_CMD_RUN)
        {
            pic_link_left_spl("RUN");
        }
    }
}


void pic_boot_init(Stream *output)
{
    if (!pic_store_flash_init())
    {
        output->printf("PIC updates: OFF, no picfw partition (the ESP needs reflashing over USB "
                       "with the current partition table)\n");
    }
    pic_link_set_frame_hook(on_frame);
}


static void print_version(Stream *output, uint32_t version)
{
    char text[16];

    pic_report_version(text, sizeof(text), version);
    output->print(text);
}


bool pic_boot_update(const PicStoredFile *file, bool force, Stream *output)
{
    if (!pic_store_present())
    {
        output->printf("PIC update: no picfw partition\n");
        return false;
    }
    if (pic_boot_updating())
    {
        output->printf("PIC update: one is running already\n");
        return false;
    }
    if (!file->valid || file->kind != PIC_FILE_APP)
    {
        output->printf("PIC update: no valid application file\n");
        return false;
    }
    staged = *file;
    started = true;
    shown = PIC_UPDATE_FAILED;
    quarter = 0;
    started_ms = millis();
    {
        char text[16];

        pic_report_version(text, sizeof(text), staged.header.version);
        console_printf("PIC update: to %s, %lu instructions%s\n", text,
                       (unsigned long)staged.header.length, force ? ", forced" : "");
    }
    pic_update_start(&update, &port, &staged.header, force, pic_link_in_spl(), started_ms);
    return true;
}


bool pic_boot_update_spl(const PicStoredFile *file, bool force, Stream *output)
{
    char text[16];

    if (!pic_store_present())
    {
        output->printf("SPL update: no picfw partition\n");
        return false;
    }
    if (pic_boot_updating())
    {
        output->printf("SPL update: a PIC update is running already\n");
        return false;
    }
    if (!file->valid || file->kind != PIC_FILE_SPL)
    {
        output->printf("SPL update: no valid SPL file\n");
        return false;
    }
    if (pic_link_in_spl())
    {
        output->printf("SPL update: the PIC is in its SPL: it is written through the "
                       "application\n");
        return false;
    }
    spl_staged = *file;
    spl_kept.valid = false;
    spl_started = true;
    spl_reinstall = false;
    spl_shown = SPL_UPDATE_FAILED;
    spl_quarter = 0;
    spl_started_ms = millis();
    pic_report_version(text, sizeof(text), spl_staged.header.version);
    console_printf("SPL update: to %s, %lu instructions%s\n", text,
                   (unsigned long)spl_staged.header.length, force ? ", forced" : "");
    spl_update_start(&spl, &spl_port, &spl_staged.header, force, spl_started_ms);
    return true;
}


bool pic_boot_updating(void)
{
    return (started && pic_update_running(&update)) || (spl_started && spl_update_running(&spl));
}


bool pic_boot_spl_result(bool *done)
{
    if (!spl_started || spl_update_running(&spl))
    {
        return false;
    }
    *done = spl.step == SPL_UPDATE_DONE;
    return true;
}


static void report(void)
{
    uint8_t now_quarter;

    if (update.step != shown)
    {
        shown = update.step;
        quarter = 0;
        switch (update.step)
        {
        case PIC_UPDATE_ENTER: console_printf("PIC update: asking the application for the SPL\n"); break;
        case PIC_UPDATE_ERASE: console_printf("PIC update: erasing\n"); break;
        case PIC_UPDATE_DATA:
            console_printf("PIC update: writing %lu chunks\n", (unsigned long)update.chunks);
            break;
        case PIC_UPDATE_VERIFY: console_printf("PIC update: reading back\n"); break;
        case PIC_UPDATE_COMMIT: console_printf("PIC update: committing\n"); break;
        case PIC_UPDATE_RUN:
            console_printf(update.already ? "PIC update: already there, starting it\n"
                                          : "PIC update: starting the new application\n");
            break;
        case PIC_UPDATE_DONE:
            finished_ms = millis();
            console_printf("PIC update: DONE, the PIC runs it, %.1f s\n",
                           (finished_ms - started_ms) / 1000.0);
            break;
        case PIC_UPDATE_FAILED:
            finished_ms = millis();
            console_printf("PIC update: FAILED: %s\n", update.reason);
            break;
        default:
            break;
        }
        if (update.passes != 0 && update.step == PIC_UPDATE_STATUS)
        {
            console_printf("PIC update: pass %u, after %s\n", (unsigned)update.passes + 1U,
                           update.reason);
        }
    }
    if ((update.step == PIC_UPDATE_DATA || update.step == PIC_UPDATE_VERIFY) && update.chunks != 0)
    {
        now_quarter = (uint8_t)(4U * update.chunk / update.chunks);
        if (now_quarter > quarter && now_quarter < 4U)
        {
            quarter = now_quarter;
            console_printf("PIC update: %s %u%%\n",
                           update.step == PIC_UPDATE_DATA ? "written" : "read back", 25U * quarter);
        }
    }
}


// The SPL it kept, else another valid SPL file that is not the rejected
// one: installed, forced, once.
static void spl_put_back(void)
{
    PicStoredFile file;
    uint8_t slot;
    PicFileHeader rejected = spl_staged.header;

    if (spl_reinstall)
    {
        console_printf("SPL update: the SPL put back was rejected too: the IPL starts the "
                       "application directly\n");
        return;
    }
    file.valid = false;
    if (spl_kept.valid && pic_store_get(PIC_FILE_SPL, spl_kept.slot, &file) &&
        (file.header.version != rejected.version || file.header.crc32 != rejected.crc32))
    {
        // the kept one
    }
    else
    {
        file.valid = false;
        for (slot = 0; slot < PIC_STORE_SLOTS && !file.valid; slot++)
        {
            if (!pic_store_get(PIC_FILE_SPL, slot, &file) ||
                (file.header.version == rejected.version && file.header.crc32 == rejected.crc32))
            {
                file.valid = false;
            }
        }
    }
    if (!file.valid)
    {
        console_printf("SPL update: no other SPL staged to put back: the IPL starts the "
                       "application directly\n");
        return;
    }
    console_printf("SPL update: putting the previous SPL back\n");
    if (pic_boot_update_spl(&file, true, &Serial))
    {
        spl_reinstall = true;
    }
}


static void spl_report(void)
{
    uint8_t now_quarter;
    char text[16];

    if (spl.step != spl_shown)
    {
        spl_shown = spl.step;
        spl_quarter = 0;
        switch (spl.step)
        {
        case SPL_UPDATE_HEALTHY:
            console_printf("SPL update: waiting for the application to have run 60 s\n");
            break;
        case SPL_UPDATE_KEEP_RECORD: console_printf("SPL update: keeping the running SPL\n"); break;
        case SPL_UPDATE_ERASE: console_printf("SPL update: erasing\n"); break;
        case SPL_UPDATE_DATA:
            console_printf("SPL update: writing %lu chunks\n", (unsigned long)spl.chunks);
            break;
        case SPL_UPDATE_VERIFY: console_printf("SPL update: reading back\n"); break;
        case SPL_UPDATE_COMMIT: console_printf("SPL update: committing\n"); break;
        case SPL_UPDATE_ENTER: console_printf("SPL update: starting the new SPL\n"); break;
        case SPL_UPDATE_TEST_ERASE: console_printf("SPL update: testing the new SPL\n"); break;
        case SPL_UPDATE_RUN: console_printf("SPL update: back to the application\n"); break;
        case SPL_UPDATE_WAIT_CONFIRM:
            console_printf("SPL update: confirming once the application has run 60 s\n");
            break;
        case SPL_UPDATE_DONE:
            spl_finished_ms = millis();
            pic_report_version(text, sizeof(text), spl_staged.header.version);
            if (spl.newer)
            {
                console_printf("SPL update: DONE, a newer SPL runs (force installs %s)\n", text);
            }
            else
            {
                console_printf(spl.already ? "SPL update: DONE, %s is there already\n"
                                           : "SPL update: DONE, %s confirmed, %.1f s\n",
                               text, (spl_finished_ms - spl_started_ms) / 1000.0);
            }
            break;
        case SPL_UPDATE_FAILED:
            spl_finished_ms = millis();
            console_printf("SPL update: FAILED: %s\n", spl.reason);
            break;
        case SPL_UPDATE_REJECTED:
            spl_finished_ms = millis();
            console_printf("SPL update: REJECTED: %s\n", spl.reason);
            spl_put_back();
            break;
        default:
            break;
        }
        if (spl.passes != 0 && spl.step == SPL_UPDATE_STATUS)
        {
            console_printf("SPL update: pass %u, after %s\n", (unsigned)spl.passes + 1U, spl.reason);
        }
    }
    if ((spl.step == SPL_UPDATE_DATA || spl.step == SPL_UPDATE_VERIFY) && spl.chunks != 0)
    {
        now_quarter = (uint8_t)(4U * spl.chunk / spl.chunks);
        if (now_quarter > spl_quarter && now_quarter < 4U)
        {
            spl_quarter = now_quarter;
            console_printf("SPL update: %s %u%%\n",
                           spl.step == SPL_UPDATE_DATA ? "written" : "read back", 25U * spl_quarter);
        }
    }
}


// A PIC in its SPL without a valid application (an update cut short, its
// power pulled mid-update): after RECOVER_AFTER_MS, the staged file it last
// ran, else the newest, without waiting for anyone.
static void recover(void)
{
    SML_BOOT_STATUS_PAYLOAD status;
    SML_STAT_SYS_PAYLOAD sys;
    PicStoredFile file;
    uint32_t now = millis();

    if (!pic_link_in_spl())
    {
        if (pic_link_get(SML_CLASS_STAT, SML_STAT_SYS, &sys, BOOT_STATUS_AGE_MS))
        {
            last_running = sys.pic_version;
        }
        stranded = false;
        return;
    }
    if (pic_boot_updating() || !pic_store_present() ||
        !pic_link_get(SML_CLASS_BOOT, SML_BOOT_STATUS, &status, BOOT_STATUS_AGE_MS) ||
        (status.mode != SML_BOOT_MODE_HELD && status.mode != SML_BOOT_MODE_NO_APP) ||
        (status.flags & (SML_BOOT_FLAG_IMAGE_OK | SML_BOOT_FLAG_CHECKING)) != 0U)
    {
        stranded = false;
        return;
    }
    if (!stranded)
    {
        stranded = true;
        stranded_ms = now;
        return;
    }
    if (now - stranded_ms < RECOVER_AFTER_MS || (started && now - finished_ms < RECOVER_AFTER_MS) ||
        recoveries >= RECOVER_LIMIT)
    {
        return;
    }
    stranded = false;
    recoveries++;
    if (!pic_store_pick(PIC_FILE_APP, last_running, &file))
    {
        console_printf("PIC: in its SPL without an application, and nothing staged to install\n");
        recoveries = RECOVER_LIMIT;
        return;
    }
    console_printf("PIC: in its SPL without an application: installing the staged file "
                   "(%u of %u)\n", (unsigned)recoveries, (unsigned)RECOVER_LIMIT);
    (void)pic_boot_update(&file, false, &Serial);
}


void pic_boot_service(void)
{
    if (pic_link_in_spl() && !pic_boot_updating() &&
        !pic_link_get(SML_CLASS_BOOT, SML_BOOT_STATUS, NULL, SPL_SILENT_MS))
    {
        pic_link_left_spl("no BOOT-STATUS for 5 s");
    }
    if (started)
    {
        if (pic_update_running(&update))
        {
            pic_update_service(&update, millis());
        }
        report();
    }
    if (spl_started)
    {
        if (spl_update_running(&spl))
        {
            spl_update_service(&spl, millis());
        }
        pic_link_hold(spl_update_holds_link(&spl));
        spl_report();
    }
    recover();
}


static const char *bits(char *text, size_t size, uint16_t value, const char *const *names,
                        unsigned count)
{
    unsigned index;

    text[0] = '\0';
    for (index = 0; index < count; index++)
    {
        if ((value & (1U << index)) != 0U)
        {
            size_t used = strlen(text);

            snprintf(text + used, size - used, "%s%s", used != 0 ? " " : "", names[index]);
        }
    }
    if (text[0] == '\0')
    {
        snprintf(text, size, "NONE");
    }
    return text;
}


static void print_status(Stream *output, const SML_BOOT_STATUS_PAYLOAD *s)
{
    static const char *const flags[] = {"RECORD",    "COMMITTED", "CHECKING", "IMAGE_OK",
                                        "ERASED",    "REQUESTED", "POWER_ON", "TEST_FREE"};
    static const char *const spl[] = {"RECORD", "COMMITTED", "INTACT", "CONFIRMED", "REJECTED"};
    static const char *const paths[] = {"APP_DIRECT", "UNCONFIRMED", "NO_SPL", "SPL_GIVEN_UP",
                                        "LAST_RESORT"};
    char text[96];

    output->printf("PIC: in its SPL, %s, %s\n", pic_report_boot_mode_name(s->mode),
                   bits(text, sizeof(text), s->flags, flags, 8));
    output->printf("  application ");
    print_version(output, s->app_version);
    output->printf(", %lu instructions, CRC-32 0x%08lX (found 0x%08lX)\n",
                   (unsigned long)s->app_length, (unsigned long)s->app_crc32,
                   (unsigned long)s->crc32_computed);
    output->printf("  SPL ");
    print_version(output, s->spl_version);
    output->printf(" %s; IPL %u.%u; protocol %u; DEVID 0x%04lX\n",
                   bits(text, sizeof(text), s->spl_state, spl, 5), s->ipl_version >> 8,
                   s->ipl_version & 0xFFU, s->protocol, (unsigned long)s->device_id);
    output->printf("  last reset: %s; boot %s; attempts %u, trials %u\n",
                   pic_report_reset_name(s->reset_cause),
                   bits(text, sizeof(text), s->boot, paths, 5), s->attempts, s->trials);
}


static void print_slots(Stream *output, PicFileKind kind, const char *name)
{
    uint8_t slot;

    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        PicStoredFile file;

        output->printf("  %s slot %u: ", name, slot);
        if (pic_store_get(kind, slot, &file))
        {
            print_version(output, file.header.version);
            output->printf(", %lu instructions, CRC-32 0x%08lX\n",
                           (unsigned long)file.header.length, (unsigned long)file.header.crc32);
        }
        else
        {
            output->printf("NONE\n");
        }
    }
}


void pic_boot_print(Stream *output, bool verbose)
{
    SML_BOOT_STATUS_PAYLOAD status;
    SML_STAT_SYS_PAYLOAD sys;

    if (pic_link_in_spl() && pic_link_get(SML_CLASS_BOOT, SML_BOOT_STATUS, &status,
                                          BOOT_STATUS_AGE_MS))
    {
        print_status(output, &status);
    }
    else if (pic_link_in_spl())
    {
        output->printf("PIC: in its SPL, no BOOT-STATUS for %u s\n", BOOT_STATUS_AGE_MS / 1000U);
    }
    else if (pic_link_get(SML_CLASS_STAT, SML_STAT_SYS, &sys, PIC_LINK_STATE_TIMEOUT_MS))
    {
        output->printf("PIC: application ");
        print_version(output, sys.pic_version);
        output->printf(" running\n");
    }
    else
    {
        output->printf("PIC: not heard from\n");
    }
    (void)verbose;
    if (spl_started)
    {
        output->printf("SPL update: ");
        print_version(output, spl_staged.header.version);
        if (spl_update_running(&spl))
        {
            output->printf(", %s", spl_update_step_name(spl.step));
            if (spl.step == SPL_UPDATE_DATA || spl.step == SPL_UPDATE_VERIFY)
            {
                output->printf(", chunk %lu of %lu", (unsigned long)spl.chunk,
                               (unsigned long)spl.chunks);
            }
            output->printf(", pass %u%s\n", (unsigned)spl.passes + 1U,
                           spl.kept ? ", running SPL kept" : "");
        }
        else if (spl.step == SPL_UPDATE_DONE)
        {
            output->printf(", DONE%s in %.1f s\n",
                           spl.newer ? " (a newer one runs)" : spl.already ? " (already there)" : "",
                           (spl_finished_ms - spl_started_ms) / 1000.0);
        }
        else
        {
            output->printf(", %s: %s\n", spl_update_step_name(spl.step), spl.reason);
        }
    }
    if (!started)
    {
        return;
    }
    output->printf("Update: ");
    print_version(output, staged.header.version);
    if (pic_update_running(&update))
    {
        output->printf(", %s, chunk %lu of %lu, pass %u\n", pic_update_step_name(update.step),
                       (unsigned long)update.chunk, (unsigned long)update.chunks,
                       (unsigned)update.passes + 1U);
    }
    else if (update.step == PIC_UPDATE_DONE)
    {
        output->printf(", DONE%s in %.1f s\n", update.already ? " (already there)" : "",
                       (finished_ms - started_ms) / 1000.0);
    }
    else
    {
        output->printf(", FAILED: %s\n", update.reason);
    }
}


void pic_boot_print_staged(Stream *output, bool verbose)
{
    if (!pic_store_present())
    {
        output->printf("Staged: no picfw partition\n");
        return;
    }
    output->printf("Staged:\n");
    print_slots(output, PIC_FILE_APP, "application");
    if (verbose)
    {
        print_slots(output, PIC_FILE_SPL, "SPL");
    }
}


void pic_boot_erase(Stream *output, bool application, bool spl)
{
    uint8_t slot;
    unsigned erased = 0;

    if (!pic_store_present())
    {
        output->printf("Staged: no picfw partition\n");
        return;
    }
    if (pic_boot_updating())
    {
        output->printf("Staged: not while a PIC update runs\n");
        return;
    }
    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        if (application)
        {
            erased += pic_store_erase(PIC_FILE_APP, slot) ? 1U : 0U;
        }
        if (spl)
        {
            erased += pic_store_erase(PIC_FILE_SPL, slot) ? 1U : 0U;
        }
    }
    output->printf("Staged: %u slots erased%s\n", erased,
                   application ? "; \"update check\" stages the PIC's application again" : "");
}


void pic_boot_command(Stream *output, uint8_t command)
{
    uint8_t p[SML_BOOT_COMMAND_BYTES];

    if (!pic_link_in_spl())
    {
        output->printf("PIC: not in its SPL\n");
        return;
    }
    if (pic_boot_updating())
    {
        output->printf("PIC: an update is running\n");
        return;
    }
    memset(p, 0, sizeof(p));
    command_token = command_token == 0x6FFF ? COMMAND_TOKEN : command_token + 1U;
    sml_put_u16(&p[0], command_token);
    p[2] = command;
    command_reply = output;
    pic_link_send_wire(SML_CLASS_BOOT, SML_BOOT_COMMAND, p, sizeof(p));
}
