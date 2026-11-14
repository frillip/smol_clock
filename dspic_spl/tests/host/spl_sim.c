/*
 * The SPL's core on a pipe: stdin is what the ESP sends, stdout what the PIC
 * answers, as raw SML bytes, at real time. For the tests of
 * tools/pic_boot.py, which runs it with --sim in place of a serial
 * port.
 *
 *   spl_sim <flash.bin> [--app]
 *
 * flash.bin: program memory as little-endian 32-bit words, one per
 * instruction from 0x000000 (port_host.h), read at start if it exists and
 * written back at the end. Without --app it starts as after a power-on: the
 * IPL starts the SPL. With --app, as a clock running its application.
 *
 * After a hand-over it stands in for the application: a banner in text mode,
 * then, once a frame has arrived, STAT-SYS every second and on
 * REQUEST_MESSAGE, with the version in the record, and CMD-USER
 * ENTER_BOOTLOADER and RESET answered (CMD-RESPONSE ACK, then the SPL again). A reset of the SPL starts
 * it again with the mailbox kept. Ends at the end of stdin, or on SIGTERM.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../../common/sml.h"
#include "../../spl_core.h"
#include "../../spl_port.h"
#include "port_host.h"

static BootMailbox mailbox;
static const char *flash_path;
static volatile sig_atomic_t stop;
static uint32_t started_ms;

static uint32_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U);
}


static void load(void)
{
    FILE *file = fopen(flash_path, "rb");

    if (file == NULL)
    {
        return;
    }
    if (fread(host_flash, sizeof(host_flash), 1, file) != 1)
    {
        fprintf(stderr, "spl_sim: %s: short\n", flash_path);
        exit(2);
    }
    fclose(file);
}


static void save(void)
{
    FILE *file = fopen(flash_path, "wb");

    if (file == NULL || fwrite(host_flash, sizeof(host_flash), 1, file) != 1)
    {
        fprintf(stderr, "spl_sim: cannot write %s\n", flash_path);
        exit(2);
    }
    fclose(file);
}


static void flush_out(void)
{
    uint32_t done = 0;

    while (done < host_tx_length)
    {
        ssize_t n = write(1, host_tx + done, host_tx_length - done);

        if (n < 0 && errno != EINTR)
        {
            stop = 1;
            break;
        }
        done += n > 0 ? (uint32_t)n : 0U;
    }
    host_tx_length = 0;
}


// Moves stdin into the receive queue, waiting up to wait_ms. False at its end.
static bool pump(int wait_ms)
{
    struct pollfd fd = {0, POLLIN, 0};
    uint8_t buffer[4096];

    flush_out();
    if (poll(&fd, 1, wait_ms) > 0)
    {
        ssize_t n = read(0, buffer, sizeof(buffer));

        if (n == 0)
        {
            return false;
        }
        if (n > 0)
        {
            host_rx_push(buffer, (uint32_t)n);
        }
    }
    host_ms = now_ms() - started_ms;
    return true;
}


static void spl_start(uint16_t cause, bool request)
{
    mailbox.magic = MAILBOX_MAGIC_VALUE;
    mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    mailbox.reset_cause = cause;
    mailbox.running = 0;
    if (request)
    {
        mailbox.request = MAILBOX_REQUEST_STAY;
    }
    fprintf(stderr, "spl_sim: SPL started%s\n", request ? ", requested" : "");
    spl_init(&mailbox);
}


static uint32_t app_version(void)
{
    return (host_get(APP_RECORD_ADDRESS + 2U * APP_RECORD_VERSION) & 0xFFFFU) << 16 |
           (host_get(APP_RECORD_ADDRESS + 2U * (APP_RECORD_VERSION + 1U)) & 0xFFFFU);
}


static void send_frame(uint8_t class_id, uint8_t id, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(class_id, id, payload, length, frame);

    spl_port_tx(frame, bytes);
}


// The application, as far as the tool needs it. Returns when it resets.
// True for ENTER_BOOTLOADER.
static void send_stat_sys(uint16_t cause, uint32_t start)
{
    uint8_t p[SML_STAT_SYS_BYTES] = {0};

    sml_put_u16(&p[2], cause);
    sml_put_u32(&p[4], (host_ms - start) / 1000U);
    sml_put_u32(&p[8], app_version());
    send_frame(SML_CLASS_STAT, SML_STAT_SYS, p, sizeof(p));
}


/* The application, as far as the tool needs it, as the real one starts: its
 * link in text mode, a banner as text, and frames only once a first valid
 * frame has arrived (SML mode). Then STAT-SYS every second, REQUEST_MESSAGE
 * for it answered, ENTER_BOOTLOADER and RESET. Returns when it resets: true
 * for ENTER_BOOTLOADER. */
static bool application(uint16_t cause)
{
    static const char banner[] = "\x1b[2J\x1b[1;1HPIC, up 00:00:00, last reset\r\n";
    SmlParser parser;
    SmlFrame frame;
    uint32_t start = host_ms;
    uint32_t next = host_ms;
    bool sml_mode = false;
    uint8_t byte;

    fprintf(stderr, "spl_sim: application %u.%u.%u running\n",
            (unsigned)(app_version() >> 16 & 0xFF), (unsigned)(app_version() >> 8 & 0xFF),
            (unsigned)(app_version() & 0xFF));
    sml_parser_init(&parser);
    spl_port_tx((const uint8_t *)banner, (uint16_t)(sizeof(banner) - 1U));
    while (!stop && pump(5))
    {
        if (sml_mode && (int32_t)(host_ms - next) >= 0)
        {
            send_stat_sys(cause, start);
            next += 1000U;
        }
        while (spl_port_rx(&byte))
        {
            uint16_t command;
            uint8_t r[SML_CMD_RESPONSE_BYTES] = {0};

            if (!sml_parser_push(&parser, byte, &frame))
            {
                continue;
            }
            if (!sml_mode)
            {
                sml_mode = true;
                next = host_ms + 1000U;
            }
            if (frame.class_id != SML_CLASS_CMD || frame.message_id != SML_CMD_USER ||
                frame.length != SML_CMD_USER_BYTES)
            {
                continue;
            }
            command = sml_get_u16(frame.payload);
            memcpy(&r[4], &frame.payload[4], 2); // token
            sml_put_u16(&r[6], command);
            if (command == SML_USER_REQUEST_MESSAGE &&
                sml_get_u32(&frame.payload[6]) == (SML_CLASS_STAT << 8 | SML_STAT_SYS))
            {
                send_stat_sys(cause, start);
                send_frame(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
                continue;
            }
            if (command != SML_USER_ENTER_BOOTLOADER && command != SML_USER_RESET)
            {
                sml_put_u16(&r[0], SML_RESULT_NACK);
                sml_put_u16(&r[2], SML_ERROR_UNSUPPORTED);
                send_frame(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
                continue;
            }
            send_frame(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
            flush_out();
            return command == SML_USER_ENTER_BOOTLOADER;
        }
    }
    return false;
}


static void on_signal(int number)
{
    (void)number;
    stop = 1;
}


int main(int argc, char **argv)
{
    bool app = argc == 3 && strcmp(argv[2], "--app") == 0;

    if (argc < 2 || (argc == 3 && !app) || argc > 3)
    {
        fprintf(stderr, "usage: spl_sim <flash.bin> [--app]\n");
        return 2;
    }
    flash_path = argv[1];
    host_reset();
    load();
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    started_ms = now_ms();
    memset(&mailbox, 0, sizeof(mailbox));
    if (app)
    {
        bool enter = application(0x0003);

        if (stop)
        {
            save();
            return 0;
        }
        spl_start(0x0040, enter);
    }
    else
    {
        spl_start(0x0003, false);
    }
    while (!stop)
    {
        SplAction action;

        if (!pump(0))
        {
            break;
        }
        action = spl_poll();
        if (action == SPL_ACTION_HAND_OVER)
        {
            bool enter;

            flush_out();
            save();
            enter = application(mailbox.reset_cause);
            if (stop)
            {
                break;
            }
            spl_start(0x0040, enter);
        }
        else if (action == SPL_ACTION_RESET)
        {
            flush_out();
            spl_start(0x0040, false);
        }
        else if (host_rx_pending() == 0U)
        {
            (void)pump(1);
        }
    }
    flush_out();
    save();
    return 0;
}
