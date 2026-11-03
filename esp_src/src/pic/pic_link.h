/*
 * The link to the dsPIC on UART2, in SML (common/SML_PROTOCOL.md).
 *
 * Receiving: every byte goes through the SML parser. Bytes that are not part
 * of a frame are the PIC's raw console text (a PIC in TEXT mode, e.g. just
 * after its reset) and go to the console, printable characters only. Frames:
 *   TIME-AUTH      paired with the PIC epoch edge captured on GPIO27
 *   TIME-PIC/CTRL/MEAS/GNSS, STAT-UI and any other PIC state: held, with age
 *   STAT-DEBUG     the PIC's console text, to the console
 *   CMD-USER       ESP commands from the PIC (codes 0x8000 up): run by the
 *                  handler from pic_link_set_command_handler(), answered
 *   STAT-SETTINGS  held, as the base for changing the PIC's settings
 *   CMD-RESPONSE   answers to pic_link_request() and to settings writes,
 *                  printed where they were asked
 *
 * The PIC moves to SML mode on the first valid frame it receives, so the
 * ESP sends CMD-USER SHOW (everything) at start, and its periodic frames (STAT-NET every
 * 10 s) bring a PIC back to SML mode after the PIC resets.
 *
 * The PIC's SPL: a BOOT-STATUS from it (any mode but APP) means the PIC is in
 * its bootloader. Until the next STAT-SYS from the application the link is
 * quiet: pic_link_send() and pic_link_request() send nothing but class 0xF0,
 * so no frame reaches the SPL while it writes flash. The PIC update
 * (pic_boot) sends its own frames with pic_link_send_wire().
 *
 * TIME-AUTH pairing: the label arrives a few ms after the edge it labels, so
 * it is paired with the latest captured edge if that is under 900 ms old.
 * Bytes read after the loop was held up (an HTTP request) may be older than
 * that suggests, so frames from such a backlog are not paired.
 */
#ifndef PIC_LINK_H
#define PIC_LINK_H

#include <Arduino.h>
#include <stdint.h>

extern "C"
{
#include "sml/sml.h"
}

// A PIC state message is current for this long.
#define PIC_LINK_STATE_TIMEOUT_MS 3000U

typedef struct
{
    uint32_t frames;  // valid frames from the PIC
    uint32_t unknown; // class and ID not in the protocol
    uint32_t wrong_direction;
    uint32_t wrong_length;
    uint32_t text_frames; // STAT-DEBUG
    uint32_t text_bytes;  // raw console text outside frames
    uint32_t text_dropped; // ... lines dropped: broken frames, unfinished
    uint32_t sent;        // frames sent
    uint32_t commands;    // CMD-USER from the PIC
    uint32_t requests;    // CMD-USER to the PIC
    uint32_t responses;   // CMD-RESPONSE from the PIC
    uint32_t unanswered;  // requests with no response within 5 s
    uint32_t pairings;    // TIME-AUTH paired with an edge
    uint32_t unpaired;    // ... not paired
    uint32_t backlogs;    // services that found the loop had been held up
    uint32_t restarts;    // PIC restarts seen (STAT-SYS uptime dropped)
    uint32_t held_back;   // frames not sent while the PIC was in its SPL
    uint32_t spl_entries; // times the PIC was seen entering its SPL
} PicLinkStats;

typedef struct
{
    bool valid;
    int64_t edge_local_us; // the PIC's epoch edge, local time
    int64_t edge_utc_us;   // UTC at that edge, by the PIC (label + phase)
    uint32_t uncertainty_ns;
    bool uncertainty_valid;
    uint8_t source;
    uint8_t lock_state;
} PicTime;

// Runs an ESP command (SML_USER_ESP_*). Returns SML_RESULT_* and sets
// *detail (SML_ERROR_*); sets *restart to restart the ESP after answering.
typedef uint16_t (*PicCommandHandler)(uint16_t command, uint32_t argument, uint16_t *detail,
                                      bool *restart);

// Every valid frame from the PIC, before the link handles it (pic_boot).
typedef void (*PicFrameHook)(const SmlFrame *frame);

void pic_link_init(void);
void pic_link_service(void);
// pic_link_service() from inside long blocking work (a download): frames are
// read and kept, so the receive buffer does not overflow, but a command from
// the PIC is answered BUSY rather than carried out there.
void pic_link_poll(void);
void pic_link_set_command_handler(PicCommandHandler handler);
void pic_link_set_frame_hook(PicFrameHook hook);

// The PIC is in its SPL: a BOOT-STATUS from it, no STAT-SYS since.
bool pic_link_in_spl(void);
// The PIC has left its SPL without its application saying so yet (its link
// starts in TEXT mode and sends nothing until a frame arrives): the link's
// own traffic resumes, and its STAT-SYS follows. why is printed.
void pic_link_left_spl(const char *why);
// While an SPL update waits on the application's answer (its flash may be
// busy, the CPU stalled): nothing but class 0xF0 goes out, as in the SPL.
void pic_link_hold(bool on);
// A frame whose payload is already in wire order, sent whatever the mode.
void pic_link_send_wire(uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                        uint16_t length);

// The PIC has sent SML frames recently (it is in SML mode).
bool pic_link_is_sml(void);
// One frame from a native payload struct.
void pic_link_send(uint8_t class_id, uint8_t message_id, const void *native);
// A command for the PIC (SML_USER_*); its answer is printed to reply_to
// (none if NULL). For SHOW and REQUEST_MESSAGE what was asked for is the
// answer, so an ACK is printed only if verbose; a request for STAT-SOURCES
// prints the sources table, in full if verbose. Returns the token.
uint16_t pic_link_request(uint16_t command, uint32_t argument, Stream *reply_to,
                          bool verbose = false);

// The PIC's settings, as last known (STAT-SETTINGS, with the writes made
// since): false until the PIC has sent them (they are then asked for).
bool pic_link_settings(SML_SETTINGS_BODY *body);
// Writes them whole (CMD-SETTINGS) against the revision last seen, the
// answer printed to reply_to. A write made while the previous one is
// unanswered is sent once that answer brings the new revision, so changes
// in quick succession all land. False, with nothing sent, if the settings
// are not known yet (they are asked for).
bool pic_link_write_settings(const SML_SETTINGS_BODY *body, Stream *reply_to);

// The latest of each PIC message, and its age; false if never received or
// older than max_age_ms. native may be NULL, to ask only whether it is held.
bool pic_link_get(uint8_t class_id, uint8_t message_id, void *native, uint32_t max_age_ms);
// The PIC's time at its latest paired epoch edge.
bool pic_link_time(PicTime *time);
// UTC now by the PIC's latest paired edge, if no older than 10 s.
bool pic_link_utc_now(uint32_t *utc_seconds);

HardwareSerial *pic_link_uart(void);
const PicLinkStats *pic_link_stats(void);
const SmlParserStats *pic_link_parser_stats(void);

#endif // PIC_LINK_H
