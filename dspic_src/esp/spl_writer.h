#ifndef SPL_WRITER_H
#define SPL_WRITER_H

/*
 * The application's side of the bootloader (the bootloader specification's
 * SPL update): SML class 0xF0 for region SPL, so the ESP can install a new
 * SPL through the running application, and the SPL's confirm and reject.
 *
 *   STATUS   BOOT-STATUS with mode APP, then ACK
 *   ERASE    SPL: ACCEPTED, then the record's page and the two code pages,
 *            one an epoch, checked blank, then ACK (value: pages erased)
 *   DATA     a chunk of the SPL's code, written in the next flash window,
 *            read back, then answered
 *   VERIFY   any chunk below BOOT_VERIFY_LIMIT, read at once
 *   COMMIT   SPL: ACCEPTED, the code's CRC-32 checked against the record's,
 *            the record's words 0-15 written and read back, then its commit
 *   CONFIRM  ACCEPTED, then confirm written and read back: only when this
 *            start was by an unconfirmed SPL that is intact and not rejected
 *   HOLD, RUN, RESET: UNSUPPORTED (the SPL's). APP and TEST: ARGUMENT.
 *
 * Reject is the application's own: started with SPL_GIVEN_UP and an SPL that
 * is committed, unconfirmed and not rejected, it writes reject in the first
 * flash window, and the IPL then starts the application directly until a new
 * SPL is installed.
 *
 * The invariant: ERASE is refused (STATE) until the application has run
 * healthily for APP_HEALTHY_S, so the program that is the fallback while
 * the SPL is being replaced has proved itself first. The flash routines
 * (spl_writer_port) refuse anything but the SPL's pages by themselves.
 *
 * Flash is erased and written only in the flash window (spl_writer_service:
 * the slot-700 quiet time the settings store uses), as a stall elsewhere
 * would disturb the epoch's work: at most one page erase an epoch, chunks as
 * they arrive while it is open. A request that needs flash is answered once
 * it is done, up to about a second later; the ESP waits for each answer
 * before it sends anything else. ERASE, COMMIT and CONFIRM are refused
 * (BUSY) while a settings save is pending, and the settings store waits
 * while one of them is under way (spl_writer_owns_flash).
 *
 * Portable: flash and the link go through SplWriterPort, so the host tests
 * run it against a flash model.
 */

#include <stdbool.h>
#include <stdint.h>

// What the application says of itself, asked for at each BOOT-STATUS and
// at start-up.
typedef struct
{
    uint16_t reset_cause; // SML_RESET_*
    uint16_t attempts;
    uint8_t boot; // SML_BOOT_PATH_*
    uint16_t trials;
    uint16_t healthy_s;
    bool save_pending; // a settings save is waiting for flash
} SplWriterApp;

typedef struct
{
    // The 24-bit instruction word at a program address (0xFF0000: DEVID).
    uint32_t (*read)(uint32_t address);
    // Erases the page at address. False if refused (not the SPL's) or WRERR.
    bool (*erase)(uint32_t address);
    // Two instruction words at address (a multiple of 4). False if refused
    // or WRERR.
    bool (*write)(uint32_t address, uint32_t first, uint32_t second);
    // Continues a CRC-32 over instruction words of flash, packed (NULL: done
    // here, a word at a time; the IPL's service on the chip).
    uint32_t (*crc32)(uint32_t crc, uint32_t address, uint32_t instructions);
    // A class 0xF0 message to the ESP, its wire payload.
    void (*send)(uint8_t message_id, const uint8_t *payload, uint16_t length);
    void (*app)(SplWriterApp *app);
    // A new SPL committed: the mailbox's trials zeroed, a new SPL's own
    // count (the IPL would otherwise give up on it for the last one's).
    void (*committed)(void);
} SplWriterPort;

typedef struct
{
    uint8_t spl_state; // SML_SPL_STATE_*
    uint32_t spl_version;
    uint32_t spl_crc32;
    bool session;      // the SPL region erased: DATA and COMMIT allowed
    bool reject_due;   // reject to be written at the next window
    uint32_t erases;   // pages
    uint32_t chunks;   // written
    uint32_t commits;
    uint32_t confirms;
    uint32_t rejects;
    uint32_t failures; // STORAGE and VERIFY
} SplWriterStatus;

// At start-up: reads the SPL's record, and decides on reject.
void spl_writer_init(const SplWriterPort *port);

// A class 0xF0 frame from the ESP, its wire payload.
void spl_writer_frame(uint8_t message_id, const uint8_t *payload, uint16_t length);

// Often (every 10 ms task), with the epoch and whether the flash window is
// open: the flash work that is due.
void spl_writer_service(uint32_t epoch, bool window);

// ERASE, COMMIT or CONFIRM under way: the settings store waits.
bool spl_writer_owns_flash(void);
// Any of those, a chunk waiting, or an open session: ENTER_BOOTLOADER waits.
bool spl_writer_busy(void);

const SplWriterStatus *spl_writer_status(void);

#endif // SPL_WRITER_H
