/*
 * The ESP's side of a PIC application update, through its SPL: the PIC into its SPL, checked, erased, written, read back, committed
 * and started, with every timeout and retry. The PIC only answers.
 *
 *   ENTER     CMD-USER ENTER_BOOTLOADER to the application; ACK, then
 *             BOOT-STATUS from the SPL (skipped when the PIC is already in
 *             its SPL)
 *   STATUS    STATUS: BOOT-STATUS and ACK. HOLD first if in the window.
 *             Checks the protocol, the DEVID, the region, a confirmed SPL;
 *             an image already there (same version and CRC) is only run
 *   ERASE     ERASE APP: ACCEPTED, then ACK
 *   DATA      every chunk that is not blank: DATA-RESPONSE ACK
 *   VERIFY    every chunk written, read back and compared
 *   COMMIT    the record: ACCEPTED, then ACK
 *   RUN       the hand-over
 *   WAIT_APP  STAT-SYS from the new application (asked for every 0.5 s,
 *             as its link starts in text mode), with the new version
 *
 * A failed pass (a write or read-back that went wrong, a PIC that reset, no
 * answer after the retries) starts again from STATUS, three passes at most.
 * Portable C: frames go out through PicUpdatePort, the image is read from
 * it, and the caller passes in the frames it receives and the time.
 */
#ifndef PIC_UPDATE_H
#define PIC_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "pic_file.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PIC_UPDATE_PASSES 3

typedef struct
{
    // A frame to the PIC, the payload already in wire order.
    void (*send)(void *context, uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                 uint16_t length);
    // Packed image bytes from offset (0: the first instruction).
    bool (*read)(void *context, uint32_t offset, uint8_t *data, uint32_t length);
    void *context;
} PicUpdatePort;

typedef enum
{
    PIC_UPDATE_ENTER,
    PIC_UPDATE_WAIT_SPL,
    PIC_UPDATE_STATUS,
    PIC_UPDATE_HOLD,
    PIC_UPDATE_ERASE,
    PIC_UPDATE_DATA,
    PIC_UPDATE_VERIFY,
    PIC_UPDATE_COMMIT,
    PIC_UPDATE_RUN,
    PIC_UPDATE_WAIT_APP,
    PIC_UPDATE_DONE,   // the new application runs
    PIC_UPDATE_FAILED, // see reason
} PicUpdateStep;

typedef struct
{
    PicUpdatePort port;
    PicFileHeader image;
    bool force; // install even if the same image is there, and with an unconfirmed SPL
    PicUpdateStep step;
    bool already; // the image was there already: only run
    char reason[96];
    uint32_t now;
    // The request in flight.
    uint16_t token;
    uint16_t next_token;
    bool accepted;
    uint8_t tries;
    uint32_t sent_ms;
    uint32_t timeout_ms;
    uint32_t step_ms; // when the step began
    // BOOT-STATUS for the STATUS in flight.
    bool have_status;
    uint8_t status[56];
    uint8_t passes;
    uint32_t chunk;   // the chunk in hand
    uint32_t chunks;  // in the image
    uint32_t written; // chunks written this pass
    uint8_t buffer[192];
} PicUpdate;

// From scratch. in_spl: the PIC is already in its SPL (it sent BOOT-STATUS
// and no STAT-SYS since), so ENTER is skipped.
void pic_update_start(PicUpdate *update, const PicUpdatePort *port, const PicFileHeader *image,
                      bool force, bool in_spl, uint32_t now_ms);

// Every frame from the PIC: BOOT-*, STAT-SYS and CMD-RESPONSE matter.
void pic_update_frame(PicUpdate *update, uint8_t class_id, uint8_t message_id,
                      const uint8_t *payload, uint16_t length, uint32_t now_ms);

// From the main loop: timeouts, retries, the next request.
void pic_update_service(PicUpdate *update, uint32_t now_ms);

bool pic_update_running(const PicUpdate *update);
const char *pic_update_step_name(PicUpdateStep step);

#ifdef __cplusplus
}
#endif

#endif // PIC_UPDATE_H
