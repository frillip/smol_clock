// Host tests for the GNSS stack: UBX framing, decoding, encoding, epoch
// assembly, and the GNSS service against a simulated, already configured
// receiver (time labels, leap seconds, TIM-TP association).
#define _DEFAULT_SOURCE
#include "host_uart2.h"
#include "sim_receiver.h"
#include "../../gnss/gnss.h"
#include "../../gnss/ubx_encode.h"
#include "../../gnss/ubx_parser.h"
#include "../../sync/source_gnss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;
#define CHECK(cond)                                                  \
    do                                                               \
    {                                                                \
        checks++;                                                    \
        if (!(cond))                                                 \
        {                                                            \
            failures++;                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                            \
    } while (0)

extern unsigned long long memmove_bytes;

// ---- Parser helpers -----------------------------------------------------
static int dispatched;
static uint8_t last_class, last_id;
// source_gnss_select() routes the capture: not exercised here.
void pps_capture_route(PpsInput input)
{
    (void)input;
}


static void count_handler(uint8_t c, uint8_t i, const uint8_t *p, uint16_t l, uint32_t t, void *x)
{
    (void)p;
    (void)l;
    (void)t;
    (void)x;
    dispatched++;
    last_class = c;
    last_id = i;
}


static uint16_t frame(uint8_t *out, uint8_t c, uint8_t id, uint16_t len)
{
    uint8_t payload[1100];
    for (uint16_t k = 0; k < len; k++)
        payload[k] = (uint8_t)(k * 7U + 3U);
    return ubx_encode_frame(c, id, payload, len, out, 1200);
}


static void feed(UbxParser *p, const uint8_t *b, uint32_t n, uint32_t t)
{
    for (uint32_t i = 0; i < n; i++)
        ubx_parser_feed(p, b[i], t);
}


static void test_parser(void)
{
    UbxParser p;
    uint8_t f[1200];
    uint16_t n;
    printf("parser\n");

    // False preamble with a huge length must cost one byte, not the stream.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    uint8_t junk[] = {0x11, 0xB5, 0x62, 0x55, 0x66, 0xFF, 0xF0};
    feed(&p, junk, sizeof junk, 0);
    for (int k = 0; k < 50; k++)
    {
        n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC, 20);
        feed(&p, f, n, 0);
    }
    CHECK(dispatched == 50);
    CHECK(p.stats.false_syncs >= 1);

    // Known message, wrong length: false sync.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    uint8_t bad_status[] = {0xB5, 0x62, 0x01, 0x03, 0x11, 0x00};
    feed(&p, bad_status, sizeof bad_status, 0);
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_STATUS, 16);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1);

    // Corrupt MON-VER near the buffer limit: resync work must stay linear.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    memmove_bytes = 0;
    n = frame(f, UBX_CLASS_MON, UBX_ID_MON_VER, 40 + 30 * 15);
    f[n - 1] ^= 0xFF;
    feed(&p, f, n, 0);
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC, 20);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1 && last_id == UBX_ID_NAV_TIMEUTC);
    CHECK(p.stats.checksum_errors == 1);
    // Adversarial: the corrupt frame's payload is packed with plausible
    // NAV-STATUS headers, each of which must be buffered and rejected.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    memmove_bytes = 0;
    {
        uint8_t payload[490];
        static const uint8_t fake_header[] = {0xB5, 0x62, 0x01, 0x03, 0x10, 0x00};
        for (int k = 0; k < 490; k++)
            payload[k] = fake_header[k % 6];
        n = ubx_encode_frame(UBX_CLASS_MON, UBX_ID_MON_VER, payload, 490, f, sizeof f);
        f[n - 1] ^= 0xFF;
        feed(&p, f, n, 0);
        n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC, 20);
        feed(&p, f, n, 0);
    }
    printf("  adversarial 490-byte frame: %llu bytes moved during resync\n", memmove_bytes);
    CHECK(dispatched == 1 && last_id == UBX_ID_NAV_TIMEUTC);
    CHECK(memmove_bytes < 4 * UBX_PARSER_BUFFER_BYTES);

    // An oversized MON-VER is dropped; the next frame survives.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    n = frame(f, UBX_CLASS_MON, UBX_ID_MON_VER, 40 + 30 * 20);
    feed(&p, f, n, 0);
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_CLOCK, 20);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1 && last_id == UBX_ID_NAV_CLOCK);

    // Messages the clock does not use (NAV-PVT, NAV-DOP) are passed over
    // whole, and counted by name rather than as noise.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    for (int k = 0; k < 10; k++)
    {
        n = frame(f, UBX_CLASS_NAV, 0x07, 92);
        feed(&p, f, n, 0);
        n = frame(f, UBX_CLASS_NAV, 0x04, 18);
        feed(&p, f, n, 0);
        n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_STATUS, 16);
        feed(&p, f, n, 0);
    }
    CHECK(dispatched == 10 && last_id == UBX_ID_NAV_STATUS);
    CHECK(p.stats.false_syncs == 0 && p.stats.checksum_errors == 0);
    CHECK(p.stats.unexpected_frames == 20 && p.stats.unexpected_other == 0);
    CHECK(p.stats.unexpected[0].class_id == UBX_CLASS_NAV &&
          p.stats.unexpected[0].message_id == 0x07 && p.stats.unexpected[0].length == 92 &&
          p.stats.unexpected[0].count == 10);
    CHECK(p.stats.unexpected[1].message_id == 0x04 && p.stats.unexpected[1].count == 10);
    // More kinds than the table holds: the rest are still counted.
    for (uint8_t id = 0x40; id < 0x46; id++)
    {
        n = frame(f, UBX_CLASS_NAV, id, 8);
        feed(&p, f, n, 0);
    }
    CHECK(p.stats.unexpected_frames == 26 && p.stats.unexpected_other == 4);
    // A corrupt frame of an unexpected message is noise: a false sync.
    n = frame(f, UBX_CLASS_NAV, 0x04, 18);
    f[n - 1] ^= 0xFF;
    feed(&p, f, n, 0);
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_STATUS, 16);
    feed(&p, f, n, 0);
    CHECK(dispatched == 11 && p.stats.false_syncs >= 1 && p.stats.checksum_errors == 0);

    // A stray header claiming a plausible length holds up the frames behind
    // it. If the stream then goes quiet, the timeout releases them.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    {
        uint8_t stray[] = {0xB5, 0x62, 0x01, 0x07, 0x5C, 0x00, 0x01, 0x02};
        feed(&p, stray, sizeof stray, 200);
        n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_STATUS, 16);
        feed(&p, f, n, 200);
        n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_EOE, 4);
        feed(&p, f, n, 200);
        CHECK(dispatched == 0);
        ubx_parser_check_timeout(&p, 260);
        CHECK(dispatched == 2 && last_id == UBX_ID_NAV_EOE && p.end == 0);
        CHECK(p.stats.timeouts == 1);
    }

    // The noise that broke the skip-by-length design: a VALGET header with a
    // huge length. It must cost one byte.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    uint8_t fake_valget[] = {0xB5, 0x62, 0x06, 0x8B, 0x01, 0xDA, 0x7B};
    feed(&p, fake_valget, sizeof fake_valget, 0);
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_TIMELS, 24);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1 && last_id == UBX_ID_NAV_TIMELS);

    // Timeout abandons a truncated frame.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    n = frame(f, UBX_CLASS_NAV, UBX_ID_NAV_STATUS, 16);
    feed(&p, f, 10, 100);
    ubx_parser_check_timeout(&p, 140);
    CHECK(p.end == 10);
    ubx_parser_check_timeout(&p, 150);
    CHECK(p.end == 0 && p.stats.timeouts == 1);

    // Fuzz: noise between valid frames.
    srand(1234);
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    static const uint8_t types[][3] = {{1, 2, 28},    {1, 3, 16},    {1, 0x21, 20}, {1, 0x22, 20},
                                       {1, 0x26, 24}, {0x0D, 3, 28}, {0x0D, 1, 16}};
    int sent = 0;
    for (int k = 0; k < 20000; k++)
    {
        uint8_t noise[40];
        int nn = rand() % 40;
        for (int j = 0; j < nn; j++)
            noise[j] = (rand() % 5 == 0) ? (j % 2 ? 0x62 : 0xB5) : (uint8_t)rand();
        feed(&p, noise, (uint32_t)nn, (uint32_t)k);
        int t = rand() % 7;
        n = frame(f, types[t][0], types[t][1], types[t][2]);
        feed(&p, f, n, (uint32_t)k);
        sent++;
    }
    ubx_parser_check_timeout(&p, 1000000U); // the stream goes quiet
    printf("  fuzz: %d of %d frames recovered, %lu false syncs, %lu checksum errors, "
           "%lu unexpected\n",
           dispatched, sent, (unsigned long)p.stats.false_syncs,
           (unsigned long)p.stats.checksum_errors, (unsigned long)p.stats.unexpected_frames);
    CHECK(dispatched == sent);
}


static void test_encode(void)
{
    uint8_t poll[UBX_POLL_FRAME_LENGTH];
    static const uint8_t mon_ver_poll[] = {0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34};
    UbxCfgMessage m;
    printf("encode\n");
    ubx_encode_poll(UBX_CLASS_MON, UBX_ID_MON_VER, poll);
    CHECK(memcmp(poll, mon_ver_poll, sizeof poll) == 0);

    // CFG-VALSET RAM, UART1 baud 115200, assembled by hand from the interface description.
    static const uint8_t baud_set[] = {0xB5, 0x62, 0x06, 0x8A, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x00,
                                       0x01, 0x00, 0x52, 0x40, 0x00, 0xC2, 0x01, 0x00, 0xF3, 0xA5};
    ubx_encode_valset_begin(&m, UBX_CFG_LAYER_RAM);
    CHECK(ubx_encode_valset_add(&m, UBX_CFG_UART1_BAUDRATE, 115200));
    CHECK(ubx_encode_cfg_finish(&m));
    CHECK(m.frame_length == sizeof baud_set && memcmp(m.frame, baud_set, sizeof baud_set) == 0);

    ubx_encode_valget_begin(&m, UBX_CFG_GET_LAYER_RAM);
    CHECK(!ubx_encode_cfg_finish(&m)); // no keys
    ubx_encode_valset_begin(&m, UBX_CFG_LAYER_RAM);
    for (int k = 0; k < 70; k++)
        ubx_encode_valset_add(&m, UBX_CFG_TP_PERIOD_TP1, 1);
    CHECK(m.overflowed && !ubx_encode_cfg_finish(&m));
}


// ---- TIM-TP decoding ------------------------------------------------------
static void test_time_pulse_decode(void)
{
    // Assembled by hand from the M9 interface description: towMS 345600000,
    // towSubMS 0x80000000 (0.5 ms), qErr -2345 ps, week 2440, flags UTC
    // available + qErrInvalid, refInfo GPS / USNO.
    static const uint8_t payload[16] = {0x00, 0x70, 0x99, 0x14, 0x00, 0x00, 0x00, 0x80,
                                        0xD7, 0xF6, 0xFF, 0xFF, 0x88, 0x09, 0x12, 0x30};
    UbxTimTp tp;
    uint8_t f[64];
    uint16_t n;
    UbxParser p;
    printf("time pulse\n");
    CHECK(ubx_decode_tim_tp(payload, sizeof payload, &tp));
    CHECK(tp.time_of_week_ms == 345600000UL && tp.time_of_week_sub_ms == 0x80000000UL);
    CHECK(tp.quantisation_error_ps == -2345 && tp.week == 2440);
    CHECK((tp.flags & UBX_TP_FLAGS_UTC_AVAILABLE) && (tp.flags & UBX_TP_FLAGS_QERR_INVALID));
    CHECK((tp.reference_info & UBX_TP_REFINFO_UTC_STANDARD_MASK) >>
              UBX_TP_REFINFO_UTC_STANDARD_SHIFT ==
          UBX_UTC_USNO);
    CHECK(!ubx_decode_tim_tp(payload, 15, &tp));
    // Framed by the parser at its length, rejected at any other.
    ubx_parser_init(&p, count_handler, NULL);
    dispatched = 0;
    n = ubx_encode_frame(UBX_CLASS_TIM, UBX_ID_TIM_TP, payload, 16, f, sizeof f);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1 && last_class == UBX_CLASS_TIM && last_id == UBX_ID_TIM_TP);
    n = ubx_encode_frame(UBX_CLASS_TIM, UBX_ID_TIM_TP, payload, 12, f, sizeof f);
    feed(&p, f, n, 0);
    CHECK(dispatched == 1);
}


// ---- Epoch assembly ------------------------------------------------------
static UbxNavStatus status_at(uint32_t tow)
{
    UbxNavStatus s = {0};
    s.time_of_week_ms = tow;
    s.fix_type = UBX_FIX_3D;
    s.flags = 0x0D;
    return s;
}


static UbxNavTimeutc utc_at(uint32_t tow, int64_t t, int32_t nano)
{
    UbxNavTimeutc u = {0};
    struct tm tm;
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    u.time_of_week_ms = tow;
    u.year = (uint16_t)(tm.tm_year + 1900);
    u.month = (uint8_t)(tm.tm_mon + 1);
    u.day = (uint8_t)tm.tm_mday;
    u.hour = (uint8_t)tm.tm_hour;
    u.minute = (uint8_t)tm.tm_min;
    u.second = (uint8_t)tm.tm_sec;
    u.fractional_second_ns = nano;
    u.validity_flags = 0x07;
    return u;
}


static void test_epoch(void)
{
    GnssEpochAssembler a;
    GnssPpsReference pps = {10, 1000, true};
    const int64_t t0 = 1791900000; // 2026-10-13
    printf("epoch\n");

    for (int k = 0; k < 2000; k++)
    {
        int64_t t = (int64_t)((unsigned)rand() % 4000000000u);
        struct tm tm;
        time_t tt = (time_t)t;
        gmtime_r(&tt, &tm);
        UtcCivil civil = {
            (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday,
            (uint8_t)tm.tm_hour,           (uint8_t)tm.tm_min,       (uint8_t)tm.tm_sec};
        if (utc_from_civil(&civil) != (uint32_t)t)
        {
            CHECK(0);
            break;
        }
    }

    gnss_epoch_init(&a);
    UbxNavPosllh pos = {0};
    pos.time_of_week_ms = 5000;
    pos.latitude_e7 = 42;
    gnss_epoch_add_position(&a, &pos); // arrives first, as on u-blox
    UbxNavStatus s = status_at(5000);
    UbxNavTimeutc u = utc_at(5000, t0, -150);
    CHECK(!gnss_epoch_add_status(&a, &s, 1120, pps));
    CHECK(gnss_epoch_add_time_utc(&a, &u, 1120, pps));
    const GnssEpoch *e = gnss_epoch_latest(&a);
    CHECK(e && e->time_valid && e->pps_associated && e->pps_sequence == 10 && e->utc_seconds == t0);
    CHECK(e->has_position && e->position.latitude_e7 == 42);
    UbxNavClock c = {0};
    c.time_of_week_ms = 5000;
    c.clock_bias_ns = 7;
    gnss_epoch_add_clock(&a, &c); // after publication
    CHECK(e->has_clock && e->clock.clock_bias_ns == 7);

    // Next epoch, but the PPS edge was missed: must not label edge 10 again.
    s = status_at(6000);
    u = utc_at(6000, t0 + 1, -150);
    gnss_epoch_add_status(&a, &s, 2120, pps);
    gnss_epoch_add_time_utc(&a, &u, 2120, pps);
    e = gnss_epoch_latest(&a);
    CHECK(e->time_valid && !e->pps_associated && a.stats.unassociated == 1);

    // Edge seen, but too long before the message.
    GnssPpsReference stale = {11, 2000, true};
    s = status_at(7000);
    u = utc_at(7000, t0 + 2, 0);
    gnss_epoch_add_status(&a, &s, 3100, stale);
    gnss_epoch_add_time_utc(&a, &u, 3100, stale);
    CHECK(!gnss_epoch_latest(&a)->pps_associated);

    // Rounding: 59.6 s rounds up into the next minute.
    GnssEpochAssembler b;
    gnss_epoch_init(&b);
    GnssPpsReference p2 = {1, 0, true};
    s = status_at(1000);
    u = utc_at(1000, t0 + 59 - (t0 % 60), 400000000);
    gnss_epoch_add_status(&b, &s, 100, p2);
    gnss_epoch_add_time_utc(&b, &u, 100, p2);
    CHECK(!gnss_epoch_latest(&b)->time_valid); // 400 ms off the second: misaligned
    s = status_at(2000);
    u = utc_at(2000, t0 + 59 - (t0 % 60), 999999800);
    p2.sequence = 2;
    gnss_epoch_add_status(&b, &s, 200, p2);
    gnss_epoch_add_time_utc(&b, &u, 200, p2);
    e = gnss_epoch_latest(&b);
    CHECK(e->time_valid && e->utc_seconds == t0 + 60 - (t0 % 60) &&
          e->offset_from_second_ns == -200);

    // Leap second 23:59:60.
    const UtcCivil eve_civil = {2026, 12, 31, 23, 59, 59};
    const uint32_t new_year_eve = utc_from_civil(&eve_civil);
    s = status_at(3000);
    u = utc_at(3000, new_year_eve, 0);
    u.second = 60; // 23:59:60
    p2.sequence = 3;
    gnss_epoch_add_status(&b, &s, 300, p2);
    gnss_epoch_add_time_utc(&b, &u, 300, p2);
    e = gnss_epoch_latest(&b);
    CHECK(e->leap_second && e->utc_seconds == new_year_eve && e->time_valid);

    // No fix: published but not time-valid. Abandoned epoch counted.
    s = status_at(4000);
    s.flags = 0;
    u = utc_at(4000, t0, 0);
    gnss_epoch_add_status(&b, &s, 400, p2);
    s = status_at(5000);
    gnss_epoch_add_status(&b, &s, 500, p2);
    CHECK(b.stats.abandoned == 1);
    s = status_at(5000);
    s.flags = 0;
    u = utc_at(5000, t0, 0);
    GnssEpochAssembler d;
    gnss_epoch_init(&d);
    gnss_epoch_add_status(&d, &s, 0, p2);
    gnss_epoch_add_time_utc(&d, &u, 0, p2);
    CHECK(!gnss_epoch_latest(&d)->time_valid && !gnss_epoch_latest(&d)->fix_ok);

    // NAV-EOE: marks the published epoch final; an epoch still missing a
    // required part is abandoned at once, and cannot be completed later.
    {
        UbxNavEoe end = {5000};
        GnssEpochAssembler e;
        gnss_epoch_init(&e);
        s = status_at(5000);
        u = utc_at(5000, t0, 0);
        gnss_epoch_add_status(&e, &s, 0, p2);
        gnss_epoch_add_time_utc(&e, &u, 0, p2);
        CHECK(!gnss_epoch_latest(&e)->ended);
        gnss_epoch_end(&e, &end);
        CHECK(gnss_epoch_latest(&e)->ended && e.stats.abandoned == 0);

        s = status_at(6000);
        u = utc_at(6000, t0 + 1, 0);
        end.time_of_week_ms = 6000;
        gnss_epoch_add_status(&e, &s, 1000, p2);
        gnss_epoch_end(&e, &end);
        CHECK(e.stats.abandoned == 1 && !e.building_active);
        CHECK(gnss_epoch_latest(&e)->time_of_week_ms == 5000);
        gnss_epoch_end(&e, &end); // repeated: no double count
        CHECK(e.stats.abandoned == 1);
    }
}


// ---- Service against a configured receiver ------------------------------
static uint32_t now;
static void run_until(uint32_t until)
{
    for (; now < until; now++)
    {
        sim_step(now);
        if (now % 10U == 0U)
            gnss_service(now, sim_pps_reference());
    }
}


// A receiver as provisioning will leave it: UBX at 115200, the clock's
// messages enabled.
static void boot_receiver_at(bool with_time_pulse, int64_t start_unix_seconds)
{
    SimReceiverConfig configured = {true, GNSS_OPERATING_BAUD, true, true, true, false, 120};
    host_uart2_reset();
    now = 1;
    sim_init(&configured, start_unix_seconds);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_POSLLH_UART1, 1);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_STATUS_UART1, 1);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_TIMEUTC_UART1, 1);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_CLOCK_UART1, 1);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_TIMELS_UART1, 10);
    sim_set_message_rate(UBX_CFG_MSGOUT_TIM_TP_UART1, with_time_pulse ? 1U : 0U);
    sim_set_message_rate(UBX_CFG_MSGOUT_TIM_TM2_UART1, 1);
    sim_set_message_rate(UBX_CFG_MSGOUT_NAV_EOE_UART1, 1);
    sim_set_epoch_mark_offset(37);
    gnss_init(40000000UL, now);
}


static void boot_configured_receiver(bool with_time_pulse)
{
    boot_receiver_at(with_time_pulse, 1791900000);
}


/* A leap second, end to end: NAV-TIMELS announces it, NAV-TIMEUTC reports
 * 23:59:60, and every PPS edge is labelled correctly both just after it is
 * captured (counted on from the previous epoch's label, as the clock reads
 * it) and once its own epoch has arrived. */
static void test_leap_second(int8_t change)
{
    const int64_t midnight = 1798761600; // 2027-01-01T00:00:00Z
    UtcLeap leap;
    int64_t labels[8];
    bool leaps[8];
    int n = 0, wrong = 0;
    bool announced = false;

    printf("leap second %+d\n", change);
    boot_receiver_at(false, midnight - 30);
    sim_schedule_leap(midnight, change);
    run_until(now + 1500);
    for (uint32_t end = now + 40000; now < end; now++)
    {
        sim_step(now);
        if (now % 10U == 0U)
            gnss_service(now, sim_pps_reference());
        if (now % 1000U == 1U || now % 1000U == 500U)
        {
            uint32_t utc;
            bool is_leap;
            bool ok = gnss_utc_at_pps(sim_pps_reference().sequence, now, &utc, &is_leap);
            if (!ok || utc != sim_utc_of_last_pps() || is_leap != sim_leap_of_last_pps())
            {
                wrong++;
                printf("  at %u ms: got %lld%s, edge was %lld%s\n", (unsigned)(now % 1000U),
                       (long long)utc, is_leap ? " leap" : "", (long long)sim_utc_of_last_pps(),
                       sim_leap_of_last_pps() ? " leap" : "");
            }
            // Ahead of the event, it is known as that midnight.
            if (now % 1000U == 500U && sim_utc_of_last_pps() == midnight - 10)
                announced = gnss_leap_event(now, &leap) && leap.event_utc == midnight &&
                            leap.change == change;
            if (now % 1000U == 1U && sim_utc_of_last_pps() >= midnight - 3 && n < 8)
            {
                labels[n] = utc;
                leaps[n++] = is_leap;
            }
        }
    }
    printf("  labels from 23:59:57:");
    for (int k = 0; k < n; k++)
        printf(" %+lld%s", (long long)(labels[k] - midnight), leaps[k] ? "(60)" : "");
    printf("\n");
    CHECK(wrong == 0);
    CHECK(n == 8);
    if (change > 0)
        CHECK(labels[0] == midnight - 3 && labels[2] == midnight - 1 && !leaps[2] &&
              labels[3] == midnight - 1 && leaps[3] && labels[4] == midnight);
    else
        CHECK(labels[0] == midnight - 3 && labels[1] == midnight - 2 && labels[2] == midnight);
    CHECK(announced);
}


static void test_service(void)
{
    const GnssState *g;
    const UbxTimTp *pulse;
    uint32_t utc;
    int8_t change, leap;
    int32_t to_event;
    long labelled = 0, pulses_matched = 0;
    printf("service\n");

    boot_configured_receiver(true);
    run_until(now + 1500); // first edge at 1 s, labelled ~120 ms later
    g = gnss_state();
    CHECK(g->detection.state == GNSS_DETECTION_CONFIRMED);
    CHECK(strcmp(g->detection.identity.module_name, "NEO-M9N") == 0);
    CHECK(host_uart2_baud() == GNSS_OPERATING_BAUD);

    // Every edge labelled with the right second, and every TIM-TP tied to
    // the edge it describes, with that edge's qErr.
    for (uint32_t end = now + 20000; now < end; now++)
    {
        sim_step(now);
        if (now % 10U == 0U)
            gnss_service(now, sim_pps_reference());
        if (now % 1000U == 1U) // just after an edge, as the epoch report runs
        {
            uint32_t edge = sim_pps_reference().sequence;
            CHECK(gnss_utc_at_pps(edge, now, &utc, NULL) && utc == sim_utc_of_last_pps());
            labelled++;
            if (gnss_time_pulse(edge, &pulse))
            {
                CHECK(pulse->quantisation_error_ps == sim_qerr_of_last_pps());
                pulses_matched++;
            }
        }
    }
    printf("  20 s: %ld edges labelled, %ld with their TIM-TP qErr\n", labelled, pulses_matched);
    CHECK(pulses_matched >= labelled - 1); // the first edge has none
    CHECK(gnss_is_present(now) && gnss_has_fix(now));
    CHECK(g->has_leap_seconds && gnss_leap_seconds(now, &leap) && leap == 18);
    CHECK(gnss_leap_pending(now, &change, &to_event) && change == 1 && to_event <= 3600);
    CHECK(g->time_pulses_unassociated == 0);

    // Every message the receiver sends is framed, NAV-EOE included, and the
    // latest epoch has been closed by it.
    {
        UbxParserStats stats;
        gnss_parser_stats(&stats);
        printf("  parser: %lu valid, %lu false syncs\n", (unsigned long)stats.valid_frames,
               (unsigned long)stats.false_syncs);
        CHECK(stats.false_syncs == 0 && stats.checksum_errors == 0);
        CHECK(gnss_fresh_epoch(now) != NULL && gnss_fresh_epoch(now)->ended);
    }

    // TIM-TM2: the epoch edge is the receiver's falling edge (inverted on the
    // board), 37 ns after the second; then 12 ns before it.
    {
        int32_t offset;
        uint32_t accuracy;
        CHECK(gnss_mark_offset(now, &offset, &accuracy) && offset == 37 && accuracy == 45);
        sim_set_epoch_mark_offset(-12);
        run_until(now + 1000);
        CHECK(gnss_mark_offset(now, &offset, &accuracy) && offset == -12);
        CHECK(g->time_marks_out_of_window == 0);

        // A latch pulse mid-second (V2: the display shares EXTINT) is not
        // the epoch edge, either side of the second.
        sim_set_epoch_mark_offset(250000000L);
        run_until(now + 1000);
        CHECK(!gnss_mark_offset(now, &offset, &accuracy));
        sim_set_epoch_mark_offset(-1000001L);
        run_until(now + 1000);
        CHECK(!gnss_mark_offset(now, &offset, &accuracy));
        CHECK(g->time_marks_out_of_window >= 2);
        sim_set_epoch_mark_offset(999999L);
        run_until(now + 1000);
        CHECK(gnss_mark_offset(now, &offset, &accuracy) && offset == 999999L);
        sim_set_epoch_mark_offset(37);
    }

    // The clock's view of GNSS as a time source (sync/source_gnss.c): the
    // edge captured in our epoch, its label, tAcc and the TM2 cross-check.
    {
        TimingSnapshot snap;
        SyncSourceReport report;
        run_until(now + 2000 - now % 1000U + 1U); // just after an edge, TM2 settled
        memset(&snap, 0, sizeof snap);
        // Watched, and captured too: the capture 1 us before the watch.
        snap.monitor.inputs[PPS_INPUT_GNSS].valid = true;
        snap.monitor.inputs[PPS_INPUT_GNSS].sequence = sim_pps_reference().sequence;
        snap.monitor.inputs[PPS_INPUT_GNSS].stamp = 1000040UL;
        snap.capture.reference_input = PPS_INPUT_GNSS;
        snap.capture.reference_capture_valid = true;
        snap.capture.reference_capture = 1000000UL;
        source_gnss_report(&snap, now, &report);
        CHECK(report.present && report.time_valid && report.edge && report.labelled);
        CHECK(report.captured && report.edge_stamp == 1000000UL);
        CHECK(report.utc_seconds == sim_utc_of_last_pps() && !report.leap_second);
        CHECK(report.edge_sequence == sim_pps_reference().sequence);
        CHECK(report.uncertainty_ns == 20U);
        CHECK(report.cross_check && report.cross_check_ns == 37);
        CHECK(report.leap.change == 0); // the sim's default TIMELS is not a midnight
        // Routed elsewhere (NTP selected): watched only, still labelled.
        snap.capture.reference_input = PPS_INPUT_ESP;
        source_gnss_report(&snap, now, &report);
        CHECK(report.edge && report.labelled && !report.captured);
        CHECK(report.edge_stamp == 1000040UL - PPS_MONITOR_LATENCY_TICKS);
        snap.monitor.inputs[PPS_INPUT_GNSS].valid = false;
        source_gnss_report(&snap, now, &report);
        CHECK(report.present && !report.edge && !report.labelled);
    }

    // Receiver goes unlocked: no PPS, time stops being usable, and qErr is
    // flagged invalid so no edge gets one.
    sim_set_locked(false);
    run_until(now + 3000);
    CHECK(!gnss_utc_at_pps(sim_pps_reference().sequence, now, &utc, NULL));
    {
        TimingSnapshot snap;
        SyncSourceReport report;
        memset(&snap, 0, sizeof snap);
        source_gnss_report(&snap, now, &report);
        CHECK(report.present && !report.time_valid);
    }
    CHECK(!gnss_time_pulse(sim_pps_reference().sequence + 1U, &pulse));
    sim_set_locked(true);
    run_until(now + 2000);
    CHECK(gnss_utc_at_pps(sim_pps_reference().sequence, now, &utc, NULL) &&
          utc == sim_utc_of_last_pps());

    // One missing pulse while locked must never mislabel the second, nor
    // attach a TIM-TP to the wrong edge.
    sim_drop_next_pps();
    for (uint32_t end = now + 3000; now < end; now++)
    {
        sim_step(now);
        if (now % 10U == 0U)
        {
            uint32_t edge = sim_pps_reference().sequence;
            gnss_service(now, sim_pps_reference());
            if (gnss_utc_at_pps(edge, now, &utc, NULL))
                CHECK(utc == sim_utc_of_last_pps());
            if (gnss_time_pulse(edge, &pulse))
                CHECK(pulse->quantisation_error_ps == sim_qerr_of_last_pps());
        }
    }

    // Receiver without TIM-TP enabled: everything else still works.
    boot_configured_receiver(false);
    run_until(now + 3000);
    CHECK(gnss_utc_at_pps(sim_pps_reference().sequence, now, &utc, NULL) &&
          utc == sim_utc_of_last_pps());
    CHECK(!gnss_state()->has_time_pulse);

    // Absent receiver: detection gives up, nothing claims to be present.
    {
        SimReceiverConfig absent = {false, GNSS_OPERATING_BAUD, true, true, true, false, 120};
        host_uart2_reset();
        now = 1;
        sim_init(&absent, 1791900000);
        gnss_init(40000000UL, now);
        run_until(now + 3000);
        CHECK(gnss_state()->detection.state == GNSS_DETECTION_FAILED);
        CHECK(!gnss_is_present(now));
    }
}


int main(void)
{
    test_parser();
    test_encode();
    test_time_pulse_decode();
    test_epoch();
    test_service();
    test_leap_second(+1);
    test_leap_second(-1);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
