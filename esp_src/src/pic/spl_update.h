/*
 * The ESP's side of a PIC SPL update (the bootloader specification, SPL
 * update): a new second-stage loader written through the running
 * application, then tested and confirmed. The PIC only answers.
 *
 *   STATUS      the application's BOOT-STATUS: protocol, DEVID, the IPL new
 *               enough (min_ipl), the test page free. This SPL already,
 *               intact and confirmed, and not started around: done. Unless
 *               forced, a newer one so: done too
 *   HEALTHY     STATUS again every 5 s until the application has run 60 s
 *   KEEP        the running SPL read back (VERIFY), record and code, into a
 *               staged file, if none holds it: the one to go back to
 *   ERASE       SPL: ACCEPTED, then ACK once its three pages are erased, one
 *               an epoch in the application's flash window
 *   DATA        every chunk that is not blank
 *   VERIFY      every chunk written, read back and compared
 *   COMMIT      SPL: ACCEPTED, then ACK
 *   CHECK       STATUS: the new SPL committed and intact
 *   ENTER       ENTER_BOOTLOADER: the IPL starts the new SPL, unconfirmed,
 *               held. If it never comes up, the IPL gives up on it after its
 *               trials and starts the application, which rejects it
 *   TEST        through the new SPL, the test page: ERASE, a few chunks of
 *               DATA, VERIFY, COMMIT, then ERASE again, so it is shown to
 *               erase, write and commit before anything depends on it
 *   RUN         the hand-over, and STAT-SYS from the application
 *   CONFIRM     once the application has run APP_CONFIRM_S (10 s; STATUS
 *               every 2 s): CONFIRM, and the SPL is the one the IPL trusts.
 *               Refused (STATE) by an older application: again at 60 s
 *
 * A failed pass before COMMIT (a write that went wrong, no answer) starts
 * again from STATUS, three passes at most; the clock runs its application
 * meanwhile, started by the IPL directly while no SPL is intact. A new SPL
 * that does not come up or fails its test ends REJECTED: the caller installs
 * the one it kept, forced.
 *
 * Portable C, as pic_update.c.
 */
#ifndef SPL_UPDATE_H
#define SPL_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "pic_file.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SPL_UPDATE_PASSES 3

typedef struct
{
    void (*send)(void *context, uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                 uint16_t length);
    // Packed image bytes from offset (0: the first instruction).
    bool (*read)(void *context, uint32_t offset, uint8_t *data, uint32_t length);
    // Keeping the running SPL (NULL: not kept). stored: a staged file holds
    // this version and CRC-32 already. Then the file, header first (begin),
    // the packed image in order (write), and end, which checks it.
    bool (*stored)(void *context, uint32_t version, uint32_t crc32);
    bool (*keep_begin)(void *context, const PicFileHeader *header);
    bool (*keep_write)(void *context, const uint8_t *data, uint32_t length);
    bool (*keep_end)(void *context);
    void *context;
} SplUpdatePort;

typedef enum
{
    SPL_UPDATE_STATUS,
    SPL_UPDATE_HEALTHY,
    SPL_UPDATE_KEEP_RECORD,
    SPL_UPDATE_KEEP,
    SPL_UPDATE_ERASE,
    SPL_UPDATE_DATA,
    SPL_UPDATE_VERIFY,
    SPL_UPDATE_COMMIT,
    SPL_UPDATE_CHECK,
    SPL_UPDATE_ENTER,
    SPL_UPDATE_WAIT_SPL,
    SPL_UPDATE_HOLD,
    SPL_UPDATE_TEST_ERASE,
    SPL_UPDATE_TEST_DATA,
    SPL_UPDATE_TEST_VERIFY,
    SPL_UPDATE_TEST_COMMIT,
    SPL_UPDATE_TEST_CLEAN,
    SPL_UPDATE_RUN,
    SPL_UPDATE_WAIT_APP,
    SPL_UPDATE_WAIT_CONFIRM,
    SPL_UPDATE_CONFIRM,
    SPL_UPDATE_DONE,     // confirmed (or there already: already)
    SPL_UPDATE_FAILED,   // see reason; nothing new is running
    SPL_UPDATE_REJECTED, // the new SPL did not come up, or failed its test
} SplUpdateStep;

typedef struct
{
    SplUpdatePort port;
    PicFileHeader image;
    bool force;
    SplUpdateStep step;
    bool already;
    bool newer;       // already: a newer SPL runs (not forced)
    bool kept;        // the running SPL was kept as a staged file
    bool test_failed; // the new SPL failed its test: RUN, then REJECTED
    char reason[96];
    uint32_t now;
    uint16_t token;
    uint16_t next_token;
    bool accepted;
    bool sent; // the step's request is out (WAIT_SPL: nothing yet)
    uint8_t tries;
    uint32_t sent_ms;
    uint32_t timeout_ms;
    uint32_t step_ms;
    bool have_status;
    uint8_t status[56];
    uint8_t passes;
    uint16_t confirm_s; // healthy running before CONFIRM: APP_CONFIRM_S, else an older
                        // application's APP_HEALTHY_S
    uint32_t chunk;
    uint32_t chunks;
    uint32_t written;
    uint32_t keep_length; // the running SPL's, instructions
    uint8_t buffer[192];
} SplUpdate;

void spl_update_start(SplUpdate *update, const SplUpdatePort *port, const PicFileHeader *image,
                      bool force, uint32_t now_ms);
void spl_update_frame(SplUpdate *update, uint8_t class_id, uint8_t message_id,
                      const uint8_t *payload, uint16_t length, uint32_t now_ms);
void spl_update_service(SplUpdate *update, uint32_t now_ms);

bool spl_update_running(const SplUpdate *update);
// Talking to the application, a request out: nothing else should be sent
// to the PIC (its flash may be busy).
bool spl_update_holds_link(const SplUpdate *update);
const char *spl_update_step_name(SplUpdateStep step);

#ifdef __cplusplus
}
#endif

#endif // SPL_UPDATE_H
