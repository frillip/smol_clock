#include "gnss.h"
#include <xc.h>
#include "../console/text.h"
#include "../drivers/uart2.h"
#include "../platform/time_ms.h"
#include <stddef.h>
#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

// Static storage: the parser buffer alone is over 500 bytes, far too much
// for the dsPIC stack.
static GnssState gnss;
static UbxParser parser;
static uint32_t instruction_frequency_hz;
// PPS snapshot for the current gnss_service() call; read by the handler.
static GnssPpsReference service_pps;

// ---- Message routing -----------------------------------------------------

// Milliseconds from one time of week to another, across the week boundary.
static int32_t time_of_week_delta_ms(uint32_t from_ms, uint32_t to_ms)
{
    int32_t delta = (int32_t)(to_ms - from_ms);

    if (delta > (int32_t)(UBX_WEEK_MS / 2U))
    {
        delta -= (int32_t)UBX_WEEK_MS;
    }
    else if (delta < -(int32_t)(UBX_WEEK_MS / 2U))
    {
        delta += (int32_t)UBX_WEEK_MS;
    }
    return delta;
}


/* NAV-TIMELS gives the time to the next leap second event from its own time
 * of week. Placed against the UTC of the latest epoch (normally the same
 * time of week), that is the event as a UTC time, which must be a midnight.
 * Anything else is not trusted to count edges across. */
static void handle_leap_seconds(const uint8_t *payload, uint16_t length, uint32_t now_ms)
{
    const UbxNavTimels *leap = &gnss.leap_seconds;
    const GnssEpoch *epoch = gnss_epoch_latest(&gnss.epochs);
    int32_t to_event_s;

    if (!ubx_decode_nav_timels(payload, length, &gnss.leap_seconds))
    {
        return;
    }
    gnss.has_leap_seconds = true;
    gnss.leap_seconds_received_ms = now_ms;
    gnss.has_leap_event = false;
    if ((leap->validity_flags & UBX_TIMELS_VALID_TIME_TO_EVENT) == 0U ||
        leap->leap_second_change == 0 || epoch == NULL || !epoch->time_valid)
    {
        return;
    }
    // Seconds from the epoch to the event: a few months at most.
    to_event_s =
        leap->time_to_event_s +
        (int32_t)(time_of_week_delta_ms(epoch->time_of_week_ms, leap->time_of_week_ms) / 1000L);
    if (utc_leap_snap(epoch->utc_seconds + (uint32_t)to_event_s, GNSS_LEAP_EVENT_TOLERANCE_S,
                      &gnss.leap_event.event_utc))
    {
        gnss.leap_event.change = leap->leap_second_change;
        gnss.has_leap_event = true;
    }
}


static void handle_navigation(uint8_t message_id, const uint8_t *payload, uint16_t length,
                              uint32_t now_ms)
{
    switch (message_id)
    {
        case UBX_ID_NAV_STATUS:
        {
            UbxNavStatus status;
            if (ubx_decode_nav_status(payload, length, &status))
            {
                (void)gnss_epoch_add_status(&gnss.epochs, &status, now_ms, service_pps);
            }
            break;
        }
        case UBX_ID_NAV_TIMEUTC:
        {
            UbxNavTimeutc time_utc;
            if (ubx_decode_nav_timeutc(payload, length, &time_utc))
            {
                (void)gnss_epoch_add_time_utc(&gnss.epochs, &time_utc, now_ms, service_pps);
            }
            break;
        }
        case UBX_ID_NAV_CLOCK:
        {
            UbxNavClock clock;
            if (ubx_decode_nav_clock(payload, length, &clock))
            {
                gnss_epoch_add_clock(&gnss.epochs, &clock);
            }
            break;
        }
        case UBX_ID_NAV_POSLLH:
        {
            UbxNavPosllh position;
            if (ubx_decode_nav_posllh(payload, length, &position))
            {
                gnss_epoch_add_position(&gnss.epochs, &position);
            }
            break;
        }
        case UBX_ID_NAV_EOE:
        {
            UbxNavEoe end;
            if (ubx_decode_nav_eoe(payload, length, &end))
            {
                gnss_epoch_end(&gnss.epochs, &end);
            }
            break;
        }
        case UBX_ID_NAV_TIMELS: handle_leap_seconds(payload, length, now_ms); break;
        default: break;
    }
}


// TIM-TP describes the next pulse: the edge after the latest one captured,
// provided that edge is recent enough to be this second's.
static void handle_time_pulse(const uint8_t *payload, uint16_t length, uint32_t now_ms)
{
    GnssTimePulse *pulse = &gnss.time_pulse;

    if (!ubx_decode_tim_tp(payload, length, &pulse->message))
    {
        return;
    }
    gnss.has_time_pulse = true;
    pulse->received_ms = now_ms;
    pulse->associated = service_pps.valid && !time_ms_has_elapsed(now_ms, service_pps.observed_ms,
                                                                  GNSS_PPS_ASSOCIATION_WINDOW_MS);
    pulse->pps_sequence = service_pps.sequence + 1U;
    if (!pulse->associated)
    {
        gnss.time_pulses_unassociated++;
    }
}


// Nanoseconds from the nearest whole second, from a time of week in ms and
// the sub-millisecond part in ns.
static int32_t offset_from_second_ns(uint32_t time_of_week_ms, uint32_t sub_ms_ns)
{
    int32_t offset_ns = (int32_t)(time_of_week_ms % 1000U) * 1000000L + (int32_t)sub_ms_ns;

    if (offset_ns >= 500000000L)
    {
        offset_ns -= 1000000000L;
    }
    return offset_ns;
}


// The epoch edge of a time mark, as ns from the nearest whole second of the
// receiver's time base. False if the mark holds no new, time-valid epoch edge
// on an aligned time base (the receiver's own one is not aligned to any second).
static bool epoch_edge_offset(const UbxTimTm2 *mark, int32_t *offset_ns)
{
    uint8_t timebase = (uint8_t)((mark->flags & UBX_TM2_TIMEBASE_MASK) >> UBX_TM2_TIMEBASE_SHIFT);
#if GNSS_TIME_MARK_EPOCH_EDGE_FALLING
    const uint8_t new_edge = UBX_TM2_NEW_FALLING_EDGE;
    uint32_t edge_ms = mark->falling_time_of_week_ms;
    uint32_t edge_sub_ns = mark->falling_sub_ms_ns;
#else
    const uint8_t new_edge = UBX_TM2_NEW_RISING_EDGE;
    uint32_t edge_ms = mark->rising_time_of_week_ms;
    uint32_t edge_sub_ns = mark->rising_sub_ms_ns;
#endif

    if ((mark->flags & UBX_TM2_TIME_VALID) == 0U || (mark->flags & new_edge) == 0U ||
        timebase == UBX_TIMEBASE_RECEIVER || edge_sub_ns >= 1000000UL)
    {
        return false;
    }
    *offset_ns = offset_from_second_ns(edge_ms, edge_sub_ns);
    return true;
}


static bool within_mark_window(int32_t offset_ns)
{
    return offset_ns <= GNSS_TIME_MARK_WINDOW_NS && offset_ns >= -GNSS_TIME_MARK_WINDOW_NS;
}


// TIM-TM2 is kept whatever it holds, for diagnostics. A new epoch edge far
// from the second is not the epoch (V2: an ad-hoc display latch pulse).
static void handle_time_mark(const uint8_t *payload, uint16_t length, uint32_t now_ms)
{
    int32_t offset_ns;

    if (!ubx_decode_tim_tm2(payload, length, &gnss.time_mark))
    {
        return;
    }
    gnss.has_time_mark = true;
    gnss.time_mark_received_ms = now_ms;
    if (epoch_edge_offset(&gnss.time_mark, &offset_ns) && !within_mark_window(offset_ns))
    {
        gnss.time_marks_out_of_window++;
    }
}


static void handle_message(uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                           uint16_t length, uint32_t now_ms, void *context)
{
    (void)context;
    gnss.frame_seen = true;
    gnss.last_frame_ms = now_ms;

    if (gnss_detect_handle(&gnss.detection, class_id, message_id, payload, length, now_ms))
    {
        return;
    }
    if (class_id == UBX_CLASS_NAV)
    {
        handle_navigation(message_id, payload, length, now_ms);
    }
    else if (class_id == UBX_CLASS_TIM && message_id == UBX_ID_TIM_TP)
    {
        handle_time_pulse(payload, length, now_ms);
    }
    else if (class_id == UBX_CLASS_TIM && message_id == UBX_ID_TIM_TM2)
    {
        handle_time_mark(payload, length, now_ms);
    }
}


// ---- Link management -----------------------------------------------------

// (Re)starts UART2 at the given baud with an empty parser and polls for the
// receiver.
static void start_link(uint32_t baud, uint32_t now_ms)
{
    gnss.baud = baud;
    (void)uart2_init(instruction_frequency_hz, baud);
    ubx_parser_reset(&parser);
    gnss_detect_start(&gnss.detection, now_ms);
}


// A receiver that was talking and has gone quiet may have a wedged UART or
// parser state: restart both. Rate-limited, and harmless if it is absent.
static void recover_silent_link(uint32_t now_ms)
{
    if (!gnss.frame_seen ||
        !time_ms_has_elapsed(now_ms, gnss.last_frame_ms, GNSS_LINK_RECOVERY_MS) ||
        (gnss.link_recoveries > 0U &&
         !time_ms_has_elapsed(now_ms, gnss.last_recovery_ms, GNSS_LINK_RECOVERY_MS)))
    {
        return;
    }
    gnss.link_recoveries++;
    gnss.last_recovery_ms = now_ms;
    ubx_parser_discontinuity(&parser);
    start_link(gnss.baud, now_ms);
}


// ---- Public interface ----------------------------------------------------
void gnss_init(uint32_t instruction_hz, uint32_t now_ms)
{
    memset(&gnss, 0, sizeof(gnss));
    instruction_frequency_hz = instruction_hz;
    ubx_parser_init(&parser, handle_message, NULL);
    gnss_epoch_init(&gnss.epochs);

    IPC7bits.U2RXIP = GNSS_INTERRUPT_PRIORITY;
    IPC7bits.U2TXIP = GNSS_INTERRUPT_PRIORITY;
    IPC16bits.U2EIP = GNSS_INTERRUPT_PRIORITY;
    start_link(GNSS_OPERATING_BAUD, now_ms);
}


void gnss_service(uint32_t now_ms, GnssPpsReference pps)
{
    service_pps = pps;
    (void)ubx_parser_service(&parser, now_ms, 0U);
    gnss_detect_service(&gnss.detection, now_ms);
    recover_silent_link(now_ms);
}


const GnssState *gnss_state(void)
{
    return &gnss;
}


void gnss_parser_stats(UbxParserStats *destination)
{
    if (destination != NULL)
    {
        *destination = parser.stats;
    }
}


bool gnss_is_present(uint32_t now_ms)
{
    return gnss.frame_seen &&
           !time_ms_has_elapsed(now_ms, gnss.last_frame_ms, GNSS_LINK_SILENCE_MS);
}


const GnssEpoch *gnss_fresh_epoch(uint32_t now_ms)
{
    const GnssEpoch *epoch = gnss_epoch_latest(&gnss.epochs);

    if (epoch == NULL || time_ms_has_elapsed(now_ms, epoch->completed_ms, GNSS_EPOCH_MAX_AGE_MS))
    {
        return NULL;
    }
    return epoch;
}


bool gnss_has_fix(uint32_t now_ms)
{
    const GnssEpoch *epoch = gnss_fresh_epoch(now_ms);
    return epoch != NULL && epoch->fix_ok;
}


bool gnss_utc_at_pps(uint32_t pps_sequence, uint32_t now_ms, uint32_t *utc_seconds,
                     bool *leap_second)
{
    const GnssEpoch *epoch = gnss_fresh_epoch(now_ms);
    uint32_t edges_since_label;
    UtcLeap leap;
    bool leap_known;

    if (epoch == NULL || !epoch->time_valid || !epoch->pps_associated || utc_seconds == NULL)
    {
        return false;
    }
    edges_since_label = pps_sequence - epoch->pps_sequence; // wraps safely
    if (edges_since_label >= GNSS_EPOCH_FAIL_S)
    {
        return false;
    }
    leap_known = gnss_leap_event(now_ms, &leap);
    *utc_seconds = utc_advance(epoch->utc_seconds, epoch->leap_second, edges_since_label,
                               leap_known ? &leap : NULL, leap_second);
    return true;
}


bool gnss_leap_event(uint32_t now_ms, UtcLeap *leap)
{
    if (!gnss.has_leap_seconds || !gnss.has_leap_event ||
        time_ms_has_elapsed(now_ms, gnss.leap_seconds_received_ms, GNSS_TIMELS_MAX_AGE_MS))
    {
        return false;
    }
    if (leap != NULL)
    {
        *leap = gnss.leap_event;
    }
    return true;
}


bool gnss_leap_pending(uint32_t now_ms, int8_t *change, int32_t *seconds_to_event)
{
    const UbxNavTimels *leap = &gnss.leap_seconds;

    if (!gnss.has_leap_seconds ||
        time_ms_has_elapsed(now_ms, gnss.leap_seconds_received_ms, GNSS_TIMELS_MAX_AGE_MS) ||
        (leap->validity_flags & UBX_TIMELS_VALID_TIME_TO_EVENT) == 0U ||
        leap->leap_second_change == 0 || leap->time_to_event_s < 0)
    {
        return false;
    }
    if (change != NULL)
    {
        *change = leap->leap_second_change;
    }
    if (seconds_to_event != NULL)
    {
        // Count down from the time the message was received.
        uint32_t elapsed_s = time_ms_elapsed(now_ms, gnss.leap_seconds_received_ms) / 1000U;
        *seconds_to_event = leap->time_to_event_s - (int32_t)elapsed_s;
    }
    return true;
}


bool gnss_leap_seconds(uint32_t now_ms, int8_t *gps_minus_utc_s)
{
    if (!gnss.has_leap_seconds ||
        time_ms_has_elapsed(now_ms, gnss.leap_seconds_received_ms, GNSS_TIMELS_MAX_AGE_MS) ||
        (gnss.leap_seconds.validity_flags & UBX_TIMELS_VALID_CURRENT_LS) == 0U)
    {
        return false;
    }
    if (gps_minus_utc_s != NULL)
    {
        *gps_minus_utc_s = gnss.leap_seconds.current_leap_seconds;
    }
    return true;
}


bool gnss_mark_offset(uint32_t now_ms, int32_t *offset_ns, uint32_t *accuracy_ns)
{
    int32_t offset;

    if (!gnss.has_time_mark ||
        time_ms_has_elapsed(now_ms, gnss.time_mark_received_ms, GNSS_TIME_MARK_MAX_AGE_MS) ||
        !epoch_edge_offset(&gnss.time_mark, &offset) || !within_mark_window(offset))
    {
        return false;
    }
    if (offset_ns != NULL)
    {
        *offset_ns = offset;
    }
    if (accuracy_ns != NULL)
    {
        *accuracy_ns = gnss.time_mark.accuracy_ns;
    }
    return true;
}


bool gnss_time_pulse(uint32_t pps_sequence, const UbxTimTp **time_pulse)
{
    const GnssTimePulse *pulse = &gnss.time_pulse;

    if (!gnss.has_time_pulse || !pulse->associated || pulse->pps_sequence != pps_sequence ||
        (pulse->message.flags & UBX_TP_FLAGS_QERR_INVALID) != 0U)
    {
        return false;
    }
    if (time_pulse != NULL)
    {
        *time_pulse = &pulse->message;
    }
    return true;
}


// ---- Diagnostics ---------------------------------------------------------
static const char *fix_type_name(uint8_t fix_type)
{
    switch (fix_type)
    {
        case UBX_FIX_NONE: return "NO_FIX";
        case UBX_FIX_DR_ONLY: return "DR";
        case UBX_FIX_2D: return "2D";
        case UBX_FIX_3D: return "3D";
        case UBX_FIX_GNSS_DR: return "GNSS+DR";
        case UBX_FIX_TIME_ONLY: return "TIME_ONLY";
        default: return "?";
    }
}


static void print_epoch(const GnssEpoch *epoch, uint32_t now_ms)
{
    const UbxNavTimeutc *utc = &epoch->time_utc;

    printf("Epoch ToW %lu, %lums ago%s\r\n", (unsigned long)epoch->time_of_week_ms,
           (unsigned long)time_ms_elapsed(now_ms, epoch->completed_ms),
           epoch->ended ? ", ended" : "");
    printf("UTC: %04u-%02u-%02uT%02u:%02u:%02uZ %+ldns acc %luns%s\r\n", utc->year, utc->month,
           utc->day, utc->hour, utc->minute, utc->second, (long)utc->fractional_second_ns,
           (unsigned long)utc->time_accuracy_ns, epoch->leap_second ? " LEAP" : "");
    printf("Fix: %s ok %u, time valid %u, PPS %s (seq %lu)\r\n",
           fix_type_name(epoch->status.fix_type), epoch->fix_ok, epoch->time_valid,
           epoch->pps_associated ? "labelled" : "unlabelled", (unsigned long)epoch->pps_sequence);
    if (epoch->has_clock)
    {
        printf("Clock: bias %ldns drift %ldns/s acc %luns fAcc %lups/s\r\n",
               (long)epoch->clock.clock_bias_ns, (long)epoch->clock.clock_drift_ns_per_s,
               (unsigned long)epoch->clock.time_accuracy_ns,
               (unsigned long)epoch->clock.frequency_accuracy_ps_per_s);
    }
    if (epoch->has_position)
    {
        printf("Position: lat %ld lon %ld (1e-7 deg) MSL %ldmm hAcc %lumm\r\n",
               (long)epoch->position.latitude_e7, (long)epoch->position.longitude_e7,
               (long)epoch->position.height_msl_mm,
               (unsigned long)epoch->position.horizontal_accuracy_mm);
    }
}


// Messages the receiver sends that the clock does not use: candidates to
// disable on the receiver.
static void print_unexpected(const UbxParserStats *stats)
{
    uint16_t index;

    if (stats->unexpected_frames == 0U)
    {
        return;
    }
    printf("Unused messages (%lu):", (unsigned long)stats->unexpected_frames);
    for (index = 0; index < UBX_PARSER_UNEXPECTED_KINDS; index++)
    {
        const UbxUnexpectedMessage *entry = &stats->unexpected[index];
        if (entry->count != 0U)
        {
            printf(" %02X-%02X len %u x%lu", entry->class_id, entry->message_id, entry->length,
                   (unsigned long)entry->count);
        }
    }
    if (stats->unexpected_other != 0U)
    {
        printf(" others x%lu", (unsigned long)stats->unexpected_other);
    }
    printf("\r\n");
}


// Degrees from 1e-7 degrees, to 7 places.
static void print_degrees(int32_t e7)
{
    uint32_t magnitude = e7 < 0 ? (uint32_t)(-(e7 + 1)) + 1U : (uint32_t)e7;

    printf("%s%lu.%07lu", e7 < 0 ? "-" : "", (unsigned long)(magnitude / 10000000UL),
           (unsigned long)(magnitude % 10000000UL));
}


// The receiver, its fix and time, where it is, and leap seconds.
static void print_brief(uint32_t now_ms)
{
    const GnssEpoch *epoch = gnss_epoch_latest(&gnss.epochs);
    UtcLeap leap;
    int8_t gps_minus_utc;

    printf("GNSS: ");
    if (!gnss_is_present(now_ms))
    {
        printf("SILENT\r\n");
        return;
    }
    if (gnss.detection.state == GNSS_DETECTION_CONFIRMED)
    {
        printf("%s (FW %s, PROTVER %s)", gnss.detection.identity.module_name,
               gnss.detection.identity.firmware_version, gnss.detection.identity.protocol_version);
    }
    else
    {
        printf("unidentified, %lu baud", (unsigned long)gnss.baud);
    }
    if (epoch == NULL)
    {
        printf(", no solution\r\n");
        return;
    }
    printf(", %s%s, time %s, tAcc %lu ns", fix_type_name(epoch->status.fix_type),
           epoch->fix_ok ? " OK" : "", epoch->time_valid ? "VALID" : "INVALID",
           (unsigned long)epoch->time_utc.time_accuracy_ns);
    if (time_ms_elapsed(now_ms, epoch->completed_ms) > 3000UL)
    {
        printf(", %lu s old",
               (unsigned long)(time_ms_elapsed(now_ms, epoch->completed_ms) / 1000UL));
    }
    printf("\r\n");
    if (epoch->has_position)
    {
        printf("  Position: ");
        print_degrees(epoch->position.latitude_e7);
        printf(", ");
        print_degrees(epoch->position.longitude_e7);
        printf(", %ld m MSL, hAcc %lu m\r\n", (long)(epoch->position.height_msl_mm / 1000L),
               (unsigned long)((epoch->position.horizontal_accuracy_mm + 500UL) / 1000UL));
    }
    if (gnss_leap_seconds(now_ms, &gps_minus_utc))
    {
        printf("  Leap: GPS-UTC %d s", gps_minus_utc);
        if (gnss_leap_event(now_ms, &leap) && leap.change != 0)
        {
            printf(", %+d before ", leap.change);
            utc_print(leap.event_utc);
        }
        printf("\r\n");
    }
}


void gnss_print(uint32_t now_ms, bool verbose)
{
    const GnssEpoch *epoch = gnss_epoch_latest(&gnss.epochs);
    UbxParserStats stats = parser.stats;
    int8_t change;
    int32_t seconds_to_event;

    print_brief(now_ms);
    if (!verbose)
    {
        return;
    }
    printf("Link: %s at %lu baud, %lu recoveries\r\n",
           gnss_is_present(now_ms) ? "present" : "silent", (unsigned long)gnss.baud,
           (unsigned long)gnss.link_recoveries);
    if (gnss.detection.state == GNSS_DETECTION_CONFIRMED)
    {
        printf("Receiver: %s, FW %s, PROTVER %s\r\n", gnss.detection.identity.module_name,
               gnss.detection.identity.firmware_version, gnss.detection.identity.protocol_version);
    }
    if (epoch != NULL)
    {
        print_epoch(epoch, now_ms);
    }
    else
    {
        printf("No epoch yet\r\n");
    }
    if (gnss.has_leap_seconds)
    {
        printf("Leap seconds: GPS-UTC %d", gnss.leap_seconds.current_leap_seconds);
        if (gnss_leap_pending(now_ms, &change, &seconds_to_event))
        {
            printf(", %+d in %lds", change, (long)seconds_to_event);
        }
        printf("\r\n");
    }
    if (gnss.has_time_pulse)
    {
        printf("Time pulse: next ToW %lu, qErr %ldps%s, for edge %lu%s; %lu unassociated\r\n",
               (unsigned long)gnss.time_pulse.message.time_of_week_ms,
               (long)gnss.time_pulse.message.quantisation_error_ps,
               (gnss.time_pulse.message.flags & UBX_TP_FLAGS_QERR_INVALID) ? " (invalid)" : "",
               (unsigned long)gnss.time_pulse.pps_sequence,
               gnss.time_pulse.associated ? "" : " (unassociated)",
               (unsigned long)gnss.time_pulses_unassociated);
    }
    if (gnss.has_time_mark)
    {
        const UbxTimTm2 *mark = &gnss.time_mark;
        int32_t epoch_offset_ns;
        uint32_t accuracy_ns;

        // ToW printed as seconds: whole ms / 1000, then ms and ns as 9 digits.
        printf("Time mark: rise ToW %lu.%03lu%06lus%s, fall ToW %lu.%03lu%06lus%s, acc %luns, "
               "%lums ago\r\n",
               (unsigned long)(mark->rising_time_of_week_ms / 1000U),
               (unsigned long)(mark->rising_time_of_week_ms % 1000U),
               (unsigned long)mark->rising_sub_ms_ns,
               (mark->flags & UBX_TM2_NEW_RISING_EDGE) ? " new" : "",
               (unsigned long)(mark->falling_time_of_week_ms / 1000U),
               (unsigned long)(mark->falling_time_of_week_ms % 1000U),
               (unsigned long)mark->falling_sub_ms_ns,
               (mark->flags & UBX_TM2_NEW_FALLING_EDGE) ? " new" : "",
               (unsigned long)mark->accuracy_ns,
               (unsigned long)time_ms_elapsed(now_ms, gnss.time_mark_received_ms));
        if (gnss_mark_offset(now_ms, &epoch_offset_ns, &accuracy_ns))
        {
            printf("Epoch edge per receiver: %+ldns from the second (acc %luns)\r\n",
                   (long)epoch_offset_ns, (unsigned long)accuracy_ns);
        }
        printf("Time marks outside the epoch window: %lu\r\n",
               (unsigned long)gnss.time_marks_out_of_window);
    }
    printf("Epochs: %lu published, %lu abandoned, %lu unlabelled, %lu invalid\r\n",
           (unsigned long)gnss.epochs.stats.published, (unsigned long)gnss.epochs.stats.abandoned,
           (unsigned long)gnss.epochs.stats.unassociated,
           (unsigned long)gnss.epochs.stats.invalid_time);
    printf("UBX: %lu valid, %lu cksum err, %lu false sync, %lu timeouts, %lu discont.\r\n",
           (unsigned long)stats.valid_frames, (unsigned long)stats.checksum_errors,
           (unsigned long)stats.false_syncs, (unsigned long)stats.timeouts,
           (unsigned long)stats.discontinuities);
    if (stats.false_syncs != 0U)
    {
        printf("Last false sync: class 0x%02X id 0x%02X length %u\r\n", stats.last_false_sync_class,
               stats.last_false_sync_id, stats.last_false_sync_length);
    }
    print_unexpected(&stats);
}


void gnss_print_edge(uint32_t pps_sequence, uint32_t now_ms)
{
    const GnssEpoch *epoch = gnss_fresh_epoch(now_ms);
    const UbxTimTp *pulse;
    uint32_t utc;
    bool leap_second;
    int8_t leap;
    int32_t mark_offset_ns;
    uint32_t mark_accuracy_ns;
    TextLine line;

    // Every epoch: built with the fast formatter, not printf (see text.h).
    text_start(&line);
    text_str(&line, "      GNSS:");
    if (!gnss_is_present(now_ms))
    {
        text_str(&line, " silent\r\n");
        text_send(&line);
        return;
    }
    if (gnss_utc_at_pps(pps_sequence, now_ms, &utc, &leap_second))
    {
        text_str(&line, " UTC ");
        text_u32(&line, utc, 0, ' ');
        text_str(&line, leap_second ? " (23:59:60)" : "");
    }
    else
    {
        text_str(&line, " UTC ?");
    }
    if (epoch != NULL)
    {
        text_str(&line, " fix ");
        text_str(&line, fix_type_name(epoch->status.fix_type));
        text_str(&line, epoch->fix_ok ? "" : " (not ok)");
        text_str(&line, " tAcc ");
        text_u32(&line, epoch->time_utc.time_accuracy_ns, 0, ' ');
        text_str(&line, "ns");
    }
    if (gnss_leap_seconds(now_ms, &leap))
    {
        text_str(&line, " LS ");
        text_i32(&line, leap, 0, false);
    }
    if (gnss_time_pulse(pps_sequence, &pulse))
    {
        text_str(&line, " qErr ");
        text_i32(&line, pulse->quantisation_error_ps, 0, false);
        text_str(&line, "ps");
    }
    if (gnss_mark_offset(now_ms, &mark_offset_ns, &mark_accuracy_ns))
    {
        text_str(&line, " TM2 ");
        text_i32(&line, mark_offset_ns, 0, true);
        text_str(&line, "ns");
    }
    text_str(&line, "\r\n");
    text_send(&line);
}
