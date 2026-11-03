#include "pic_report.h"

#include "../timing/ntp.h"
#include "../timing/pps.h"
#include "../util/time_text.h"
#include "pic_link.h"

typedef struct
{
    uint16_t code;
    const char *name;
} Name;

static const Name commands[] = {
    {SML_USER_SHOW, "SHOW"},
    {SML_USER_REQUEST_MESSAGE, "REQUEST_MESSAGE"},
    {SML_USER_SAVE, "SAVE"},
    {SML_USER_DEFAULTS, "DEFAULTS"},
    {SML_USER_SET_RTC, "SET_RTC"},
    {SML_USER_SET_DEBUG, "SET_DEBUG"},
    {SML_USER_RESYNC, "RESYNC"},
    {SML_USER_RESET, "RESET"},
    {SML_USER_ENTER_BOOTLOADER, "ENTER_BOOTLOADER"},
    {SML_OPERATION_SETTINGS_WRITE, "settings"},
    {SML_USER_ESP_SAVE, "ESP_SAVE"},
    {SML_USER_ESP_DEFAULTS, "ESP_DEFAULTS"},
    {SML_USER_ESP_RESET, "ESP_RESET"},
    {SML_USER_ESP_NTP_SYNC, "ESP_NTP_SYNC"},
    {SML_USER_ESP_TZ_REMOTE, "ESP_TZ_REMOTE"},
    {SML_USER_ESP_TZ_LOOKUP, "ESP_TZ_LOOKUP"},
    {SML_USER_ESP_WIFI_FORGET, "ESP_WIFI_FORGET"},
    {SML_USER_ESP_WIFI_SETUP, "ESP_WIFI_SETUP"},
    {SML_USER_ESP_UPDATE_CHECK, "ESP_UPDATE_CHECK"},
};

static const char *lookup(const Name *names, size_t count, uint16_t code)
{
    size_t index;

    for (index = 0; index < count; index++)
    {
        if (names[index].code == code)
        {
            return names[index].name;
        }
    }
    return "?";
}

const char *pic_report_command_name(uint16_t command)
{
    return lookup(commands, sizeof(commands) / sizeof(commands[0]), command);
}

const char *pic_report_result_name(uint16_t result)
{
    static const char *const names[] = {"ACK", "NACK", "ACCEPTED"};

    return result < 3 ? names[result] : "?";
}

const char *pic_report_error_name(uint16_t error)
{
    static const char *const names[] = {"none",   "unsupported",    "length", "argument",
                                        "busy",   "stale revision", "schema", "storage",
                                        "verify", "state",          "CRC",    "not blank"};

    return error < sizeof(names) / sizeof(names[0]) ? names[error] : "?";
}

const char *pic_report_boot_mode_name(uint8_t mode)
{
    static const char *const names[] = {"WINDOW", "HELD", "NO_APP", "LOOP", "APP"};

    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "?";
}

const char *pic_report_source_name(uint8_t source)
{
    static const char *const names[] = {"INIT", "NONE", "RTC", "ESP", "NTP", "GNSS", "HOLDOVER"};

    return source < 7 ? names[source] : "?";
}

const char *pic_report_lock_name(uint8_t lock)
{
    static const char *const names[] = {"UNINITIALISED", "ACQUIRING", "LOCKED", "HOLDOVER",
                                        "FREE_RUNNING"};

    return lock < 5 ? names[lock] : "?";
}

const char *pic_report_tz_source_name(uint8_t source)
{
    static const char *const names[] = {"NONE",   "CACHE",  "GEOIP",  "GNSS",
                                        "MANUAL", "PINNED", "SPECIAL"};

    return source < 7 ? names[source] : "?";
}

const char *pic_report_reset_name(uint16_t cause)
{
    // Most telling first: a power-on also sets BOR, and a trap without a
    // handler resets through the RESET instruction (SWR). As the PIC's own.
    static const struct
    {
        uint16_t bit;
        const char *name;
    } causes[] = {
        {SML_RESET_POR, "power-on"},
        {SML_RESET_BOR, "brown-out"},
        {SML_RESET_WDTO, "watchdog"},
        {SML_RESET_TRAPR, "trap conflict"},
        {SML_RESET_IOPUWR, "illegal opcode or uninitialised W"},
        {SML_RESET_CM, "configuration mismatch"},
        {SML_RESET_EXTR, "MCLR pin"},
        {SML_RESET_SWR, "software (reset command or unhandled trap)"},
    };
    size_t index;

    for (index = 0; index < sizeof(causes) / sizeof(causes[0]); index++)
    {
        if (cause & causes[index].bit)
        {
            return causes[index].name;
        }
    }
    return "unknown";
}

void pic_report_version(char *text, size_t size, uint32_t version)
{
    snprintf(text, size, "%lu.%lu.%lu", (unsigned long)(version >> 16),
             (unsigned long)((version >> 8) & 0xFFU), (unsigned long)(version & 0xFFU));
}

static const char *sync_name(uint8_t state)
{
    static const char *const names[] = {"INIT", "FREE_RUN", "ACQUIRE", "LOCKED", "HOLDOVER"};

    return state < 5 ? names[state] : "?";
}

static const char *cause_name(uint8_t cause)
{
    static const char *const names[] = {
        "none",        "qualified",   "tracking",      "phase lost", "source lost",
        "source back", "alternative", "better source", "expired",    "resync"};

    return cause < sizeof(names) / sizeof(names[0]) ? names[cause] : "?";
}

static const char *control_name(uint8_t state)
{
    static const char *const names[] = {"INIT", "ACQUIRE", "STEP", "TRACK"};

    return state < 4 ? names[state] : "?";
}

static const char *verdict_name(uint8_t verdict)
{
    static const char *const names[] = {"-",          "absent",     "no time",      "no edge",
                                        "unlabelled", "inaccurate", "inconsistent", "good"};

    return verdict < 8 ? names[verdict] : "?";
}

static const char *fix_name(uint8_t fix)
{
    static const char *const names[] = {"NO_FIX", "DR", "2D", "3D", "GNSS+DR", "TIME_ONLY"};

    return fix < 6 ? names[fix] : "?";
}

static void print_local(Stream *output, const SML_TIME_AUTH_PAYLOAD *auth, bool verbose)
{
    SML_STAT_UI_PAYLOAD ui;
    int32_t offset;

    if (!pic_link_get(SML_CLASS_STAT, SML_STAT_UI, &ui, PIC_LINK_STATE_TIMEOUT_MS) ||
        !(ui.flags & SML_UI_LOCAL_VALID) || !(auth->flags & SML_AUTH_UTC_VALID))
    {
        return;
    }
    offset = ui.base_offset_s + ((ui.flags & SML_UI_DST_ACTIVE) ? ui.dst_offset_s : 0);
    output->printf(verbose ? "Local:  " : "Local:    ");
    time_print_local(output, auth->utc_seconds, offset);
    if (verbose)
    {
        output->printf(" (base ");
        time_print_offset(output, ui.base_offset_s);
        output->printf(", DST ");
        time_print_offset(output, ui.dst_offset_s);
        output->printf(" %s)", (ui.flags & SML_UI_DST_ACTIVE) ? "active" : "inactive");
    }
    else if (ui.flags & SML_UI_DST_ACTIVE)
    {
        output->printf(" DST");
    }
    // Warnings either way; no schedule is normal with manual offsets.
    output->printf("%s%s%s\n",
                   (verbose && !(ui.flags & SML_UI_TZ_HELD)) ? ", no schedule held" : "",
                   (ui.flags & SML_UI_TZ_STALE) ? ", STALE" : "",
                   (ui.flags & SML_UI_TZ_AMBIGUOUS) ? ", AMBIGUOUS" : "");
}

// Compact: "PIC:      0.1.0, up 00:12:31, last reset: power-on"; verbose
// adds the reset bits and the longest main-loop pass.
static void print_system(Stream *output, bool verbose)
{
    SML_STAT_SYS_PAYLOAD sys;
    char version[16];

    if (!pic_link_get(SML_CLASS_STAT, SML_STAT_SYS, &sys, 30000U))
    {
        if (verbose)
        {
            output->printf("\n=== PIC system ===\nNo STAT-SYS (needs PIC firmware with SML 1.4)\n");
        }
        return;
    }
    pic_report_version(version, sizeof(version), sys.pic_version);
    if (!verbose)
    {
        output->printf("PIC:      %s, up ", version);
        time_print_duration(output, sys.uptime_s);
        output->printf(", last reset: %s\n", pic_report_reset_name(sys.reset_cause));
        return;
    }
    output->printf("\n=== PIC system ===\n");
    output->printf("Firmware: %s\n", version);
    output->printf("Up:       ");
    time_print_duration(output, sys.uptime_s);
    output->printf(", %lu restarts seen\n", (unsigned long)pic_link_stats()->restarts);
    output->printf("Reset:    %s (0x%04X)\n", pic_report_reset_name(sys.reset_cause),
                   sys.reset_cause);
    output->printf("Loop:     longest pass %u ms in the last second\n", sys.loop_max_ms);
}

// Compact: "UTC:      <UTC> +/-120 ns, GNSS LOCKED" and the local time.
static void print_time(Stream *output, bool verbose)
{
    SML_TIME_AUTH_PAYLOAD auth;
    PicTime time;
    PpsEdge gnss;
    int64_t ntp_utc;

    if (verbose)
    {
        output->printf("\n=== PIC time ===\n");
    }
    if (!pic_link_get(SML_CLASS_TIME, SML_TIME_AUTH, &auth, PIC_LINK_STATE_TIMEOUT_MS))
    {
        output->printf("%s%s\n", verbose ? "No TIME-AUTH from the PIC" : "UTC:      unknown",
                       pic_link_is_sml() ? "" : " (not in SML mode)");
        return;
    }
    output->printf(verbose ? "UTC:    " : "UTC:      ");
    if (auth.flags & SML_AUTH_UTC_VALID)
    {
        time_print_utc(output, auth.utc_seconds);
        if (verbose)
        {
            output->printf(" + ");
            time_print_ns(output, auth.utc_phase_ns);
        }
        if (auth.flags & SML_AUTH_UNCERTAINTY_VALID)
        {
            output->printf(verbose ? ", +/-" : " +/-");
            time_print_ns(output, auth.uncertainty_ns);
        }
    }
    else
    {
        output->printf("unknown");
    }
    if (verbose)
    {
        output->printf("\nSource: %s, %s\n", pic_report_source_name(auth.source),
                       pic_report_lock_name(auth.lock_state));
    }
    else
    {
        output->printf(", %s %s\n", pic_report_source_name(auth.source),
                       pic_report_lock_name(auth.lock_state));
    }
    print_local(output, &auth, verbose);
    if (verbose && pic_link_time(&time))
    {
        if (ntp_utc_at(time.edge_local_us, &ntp_utc))
        {
            output->printf("Epoch vs NTP: ");
            time_print_ns(output, (time.edge_utc_us - ntp_utc) * 1000);
            output->printf(" (+ = PIC ahead)\n");
        }
        if (pps_gnss_latest(&gnss))
        {
            int64_t difference = time.edge_local_us - gnss.local_us;

            difference = ((difference + 500000) % 1000000 + 1000000) % 1000000 - 500000;
            output->printf("Epoch vs GNSS PPS on the ESP: ");
            time_print_ns(output, difference * 1000);
            output->printf("\n");
        }
    }
}

static void print_sync(Stream *output)
{
    SML_TIME_PIC_PAYLOAD pic;
    SML_TIME_CTRL_PAYLOAD ctrl;

    if (pic_link_get(SML_CLASS_TIME, SML_TIME_PIC, &pic, PIC_LINK_STATE_TIMEOUT_MS))
    {
        output->printf("\n=== PIC synchroniser ===\n");
        output->printf("State:  %s (was %s, %s), %lu changes\n", sync_name(pic.sync_state),
                       sync_name(pic.previous_sync_state), cause_name(pic.last_sync_cause),
                       (unsigned long)pic.sync_events);
        output->printf("Epoch:  %lu, ", (unsigned long)pic.epoch_sequence);
        if (pic.flags & SML_PIC_FREQUENCY_VALID)
        {
            output->printf("FCY %lu.%06lu MHz, ", (unsigned long)(pic.fosc_hz / 1000000),
                           (unsigned long)(pic.fosc_hz % 1000000));
        }
        if (pic.flags & SML_PIC_PHASE_VALID)
        {
            output->printf("phase %ld cycles, ", (long)pic.phase_cycles);
        }
        output->printf("%lu missing PPS", (unsigned long)pic.missing_pps_events);
        if (pic.second_source != SML_SOURCE_INIT && pic.second_source != SML_SOURCE_NONE)
        {
            output->printf(", second choice %s", pic_report_source_name(pic.second_source));
        }
        output->printf("\n");
    }
    if (pic_link_get(SML_CLASS_TIME, SML_TIME_CTRL, &ctrl, PIC_LINK_STATE_TIMEOUT_MS))
    {
        output->printf("Loop:   %s", control_name(ctrl.controller_state));
        if (ctrl.flags & SML_CTRL_ESTIMATE_VALID)
        {
            output->printf(", oscillator %+.4f ppm", (float)ctrl.frequency_offset_q16 / 65536.0f /
                                                         (float)ctrl.nominal_period_ticks * 1e6f);
        }
        output->printf(", %lu gate rejections\n", (unsigned long)ctrl.gate_rejections);
    }
}

// Compact: the loop's oscillator estimate, as chronyc tracking's frequency.
static void print_oscillator(Stream *output)
{
    SML_TIME_CTRL_PAYLOAD ctrl;

    if (pic_link_get(SML_CLASS_TIME, SML_TIME_CTRL, &ctrl, PIC_LINK_STATE_TIMEOUT_MS) &&
        (ctrl.flags & SML_CTRL_ESTIMATE_VALID))
    {
        output->printf("Freq:     %+.4f ppm (%s)\n",
                       (float)ctrl.frequency_offset_q16 / 65536.0f /
                           (float)ctrl.nominal_period_ticks * 1e6f,
                       control_name(ctrl.controller_state));
    }
}

static void print_gnss(Stream *output, bool verbose)
{
    SML_TIME_GNSS_PAYLOAD gnss;

    if (!pic_link_get(SML_CLASS_TIME, SML_TIME_GNSS, &gnss, PIC_LINK_STATE_TIMEOUT_MS))
    {
        return;
    }
    if (!verbose)
    {
        // Compact: only a receiver that is there.
        if (gnss.flags & SML_GNSS_DETECTED)
        {
            output->printf("GNSS:     %s%s, tAcc ", fix_name(gnss.fix_type),
                           (gnss.flags & SML_GNSS_FIX_OK) ? " OK" : "");
            time_print_ns(output, gnss.utc_accuracy_ns);
            output->printf("\n");
        }
        return;
    }
    output->printf("\n=== GNSS (PIC) ===\n");
    if (!(gnss.flags & SML_GNSS_DETECTED))
    {
        output->printf("No receiver detected\n");
        return;
    }
    output->printf("Fix:    %s%s, UTC %s, tAcc ", fix_name(gnss.fix_type),
                   (gnss.flags & SML_GNSS_FIX_OK) ? " OK" : "",
                   (gnss.flags & SML_GNSS_UTC_VALID) ? "valid" : "invalid");
    time_print_ns(output, gnss.utc_accuracy_ns);
    output->printf("\n");
    if (gnss.flags & SML_GNSS_POSITION_VALID)
    {
        output->printf("Position: %.7f %.7f, %.1f m MSL, hAcc %.1f m\n",
                       (float)gnss.latitude_e7 / 1e7f, (float)gnss.longitude_e7 / 1e7f,
                       (float)gnss.height_msl_mm / 1000.0f,
                       (float)gnss.horizontal_accuracy_mm / 1000.0f);
    }
    if (gnss.flags & SML_GNSS_LEAP_VALID)
    {
        output->printf("Leap:   TAI-UTC %d s", gnss.gps_utc_s + 19);
        if (gnss.flags & SML_GNSS_LEAP_EVENT_VALID)
        {
            output->printf(", %+d at ", gnss.leap_change);
            time_print_utc(output, gnss.leap_event_utc);
        }
        output->printf("\n");
    }
}

// A bound in the units that suit it, or "-".
static void print_bound(Stream *output, bool valid, uint32_t ns)
{
    if (!valid || ns == UINT32_MAX)
    {
        output->printf("%11s", "-");
        return;
    }
    char text[TIME_NS_TEXT];

    time_format_ns(text, ns, false);
    output->printf("%11s", text);
}

// A phase, signed, in the units that suit it, or "-".
static void print_phase(Stream *output, bool valid, int32_t ns)
{
    char text[TIME_NS_TEXT];

    if (!valid)
    {
        output->printf(" %11s", "-");
        return;
    }
    time_format_ns(text, ns, true);
    output->printf(" %11s", text);
}

static void print_age(Stream *output, uint32_t seconds)
{
    char text[TIME_AGE_TEXT];

    time_format_age(text, seconds);
    output->printf("%9s", text);
}

static bool real_source(uint8_t source)
{
    return source != SML_SOURCE_NONE && source != SML_SOURCE_INIT;
}

// Never seen and not a candidate: nothing to say about it unless asked.
static bool idle_entry(const SML_SOURCE_ENTRY *entry)
{
    return entry->rank == 0 && entry->age_s == UINT32_MAX &&
           (entry->verdict == SML_VERDICT_NONE || entry->verdict == SML_VERDICT_ABSENT);
}

// "LOCKED on GNSS, synchronised": the state and what it is steered by.
static void print_sources_state(Stream *output, const SML_STAT_SOURCES_PAYLOAD *sources,
                                bool verbose)
{
    output->printf("%s", sync_name(sources->sync_state));
    if (real_source(sources->selected) && sources->selected != SML_SOURCE_HOLDOVER)
    {
        output->printf(" on %s", pic_report_source_name(sources->selected));
    }
    if (verbose && real_source(sources->second))
    {
        output->printf(", second choice %s", pic_report_source_name(sources->second));
    }
    if (real_source(sources->challenger))
    {
        output->printf(", %s challenging", pic_report_source_name(sources->challenger));
        if (verbose)
        {
            output->printf(" for %u epochs", sources->challenge_run);
        }
    }
    output->printf("%s%s", (sources->flags & SML_SOURCES_SYNCHRONISED) ? ", synchronised" : "",
                   (sources->flags & SML_SOURCES_FALLBACK) ? ", fallback" : "");
}

/* chronyc's markers: * in use, + the second choice, x discounted: the PIC
 * has the source's time but judged it unfit, beyond its accuracy limit
 * (INACCURATE) or not following on from its own last label (INCONSISTENT);
 * ? unusable: nothing to judge (absent, no time, no edge, unlabelled, or
 * not judged yet, holdover with nothing to hold included). A source taken
 * as a fallback while INACCURATE is in use: *. */
static char source_marker(const SML_STAT_SOURCES_PAYLOAD *sources, const SML_SOURCE_ENTRY *entry)
{
    if (entry->source == sources->selected)
    {
        return '*';
    }
    if (entry->verdict == SML_VERDICT_INACCURATE || entry->verdict == SML_VERDICT_INCONSISTENT)
    {
        return 'x';
    }
    if (entry->verdict != SML_VERDICT_GOOD)
    {
        return '?';
    }
    return entry->source == sources->second ? '+' : ' ';
}

static void print_source_row(Stream *output, const SML_STAT_SOURCES_PAYLOAD *sources,
                             const SML_SOURCE_ENTRY *entry, bool verbose)
{
    bool holdover = entry->source == SML_SOURCE_HOLDOVER;

    output->printf("%c ", source_marker(sources, entry));
    if (verbose)
    {
        output->printf("%-3s ", entry->rank != 0 ? String(entry->rank).c_str() : "-");
    }
    output->printf("%-9s %-12s", pic_report_source_name(entry->source),
                   verdict_name(entry->verdict));
    print_bound(output, (entry->flags & SML_ENTRY_BOUND_VALID) != 0, entry->bound_ns);
    if (verbose)
    {
        output->printf(" ");
        print_bound(output, entry->uncertainty_ns != 0, entry->uncertainty_ns);
    }
    print_phase(output, (entry->flags & SML_ENTRY_PHASE_VALID) != 0, entry->phase_ns);
    if (verbose)
    {
        if (holdover)
        {
            output->printf(" %5s", "-");
        }
        else
        {
            output->printf(" %5u", entry->good_run);
        }
    }
    print_age(output, entry->age_s);
    if (verbose && !holdover)
    {
        output->printf("  %s%s", (entry->flags & SML_ENTRY_EDGE) ? "edge" : "no edge",
                       (entry->flags & SML_ENTRY_CAPTURED) ? ", captured" : "");
    }
    output->printf("\n");
}

/*
 * Like chronyc sources: * marks the source in use, + the second choice,
 * x one discounted, ? one unusable (source_marker).
 * Ranked entries first, then the rest in the order sent. Without verbose,
 * sources never seen are left out, as are the rank, the source's own
 * figure, the run and the edge.
 */
static void print_sources_table(Stream *output, const SML_STAT_SOURCES_PAYLOAD *sources,
                                bool verbose)
{
    uint8_t rank;
    uint8_t index;
    bool listed[SML_SOURCES_ENTRIES] = {false};

    if (verbose)
    {
        output->printf("  Rank Source    Verdict            Bound     Claimed       Phase   Run"
                       "  Last good\n");
    }
    else
    {
        output->printf("  Source    Verdict            Bound       Phase  Last good\n");
    }
    for (rank = 1; rank <= SML_SOURCES_ENTRIES + 1; rank++)
    {
        for (index = 0; index < SML_SOURCES_ENTRIES; index++)
        {
            const SML_SOURCE_ENTRY *entry = &sources->entries[index];
            bool last_pass = rank == SML_SOURCES_ENTRIES + 1;

            if (listed[index] || !real_source(entry->source) || (!last_pass && entry->rank != rank))
            {
                continue;
            }
            listed[index] = true;
            if (!verbose && idle_entry(entry))
            {
                continue;
            }
            print_source_row(output, sources, entry, verbose);
        }
    }
}

void pic_report_print_sources(Stream *output, bool verbose)
{
    SML_STAT_SOURCES_PAYLOAD sources;

    if (!pic_link_get(SML_CLASS_STAT, SML_STAT_SOURCES, &sources, 15000U))
    {
        output->printf("No STAT-SOURCES from the PIC (needs PIC firmware with SML 1.3)\n");
        return;
    }
    if (verbose)
    {
        output->printf("\n=== PIC sources ===\nState:  ");
    }
    else
    {
        output->printf("Sync:     ");
    }
    print_sources_state(output, &sources, verbose);
    output->printf("\n");
    print_sources_table(output, &sources, verbose);
}

void pic_report_print_link(Stream *output)
{
    const PicLinkStats *stats = pic_link_stats();
    const SmlParserStats *parser = pic_link_parser_stats();

    output->printf("\n=== PIC link ===\n");
    output->printf("Mode:   %s\n", pic_link_is_sml() ? "SML" : "no SML frames (TEXT or silent)");
    output->printf("Frames: %lu in (%lu text, %lu unknown, %lu wrong length), %lu out\n",
                   (unsigned long)stats->frames, (unsigned long)stats->text_frames,
                   (unsigned long)stats->unknown, (unsigned long)stats->wrong_length,
                   (unsigned long)stats->sent);
    output->printf("Parser: %lu noise bytes, %lu CRC errors, %lu length errors, %lu timeouts\n",
                   (unsigned long)parser->noise_bytes, (unsigned long)parser->crc_errors,
                   (unsigned long)parser->length_errors, (unsigned long)parser->timeouts);
    output->printf("Text:   %lu bytes printed, %lu lines dropped (broken frames, unfinished)\n",
                   (unsigned long)stats->text_bytes, (unsigned long)stats->text_dropped);
    output->printf("Commands: %lu to the PIC, %lu answered, %lu unanswered; %lu from it\n",
                   (unsigned long)stats->requests, (unsigned long)stats->responses,
                   (unsigned long)stats->unanswered, (unsigned long)stats->commands);
    output->printf("Epoch pairing: %lu paired, %lu not, %lu backlogs\n",
                   (unsigned long)stats->pairings, (unsigned long)stats->unpaired,
                   (unsigned long)stats->backlogs);
}

void pic_report_print(Stream *output, bool verbose)
{
    if (!verbose)
    {
        print_system(output, false);
    }
    print_time(output, verbose);
    if (verbose)
    {
        print_sync(output);
    }
    else
    {
        print_oscillator(output);
    }
    if (pic_link_get(SML_CLASS_STAT, SML_STAT_SOURCES, NULL, 15000U))
    {
        pic_report_print_sources(output, verbose);
    }
    print_gnss(output, verbose);
    if (verbose)
    {
        print_system(output, true);
        pic_report_print_link(output);
    }
}
