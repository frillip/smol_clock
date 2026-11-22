#include "ntp.h"

#include <stdarg.h>

#include <Network.h>
#include <esp_timer.h>
#include <math.h>
#include <strings.h>
#include <sys/time.h>

#include "../console/console.h"
#include "../net/dhcp_ntp.h"
#include "../net/net_connect.h"
#include "../settings/user_prefs.h"
#include "../util/time_text.h"
#include "esp_clock.h"
#include "ntp_client.h"
#include "ntp_select.h"
#include "ntp_stats.h"
#include "pps.h"
#include "temp_comp.h"

#define LEARN_FIRST_S     64U
#define POLL_MIN_S        128U // adaptive range, once learnt: from here ...
#define POLL_START_S      256U
#define POLL_TARGET       8U // samples in the followed server's line (chronyd's polltarget)
#define RETRY_FIRST_S     60U
// Kiss-o'-death (RFC 5905 7.4): RATE doubles that server's interval, up to
// a day, halved again after RATE_EASE_POLLS good polls; DENY and RSTR stop
// polling it until its setting changes, the network comes back, ntp sync,
// or a week (NTP_REFRESH_S) has passed.
#define RATE_MAX_S        86400U
#define RATE_EASE_POLLS   8U
// Server 0 is the one DHCP offers (ntp.dhcp), 1 to NTP_SERVERS those set.
#define NTP_SLOTS  (NTP_SERVERS + 1)
#define DHCP_SLOT  0
#define DHCP_HOST  "dhcp"
#define NTP_RESOLVE_FAILURES 2U     // failed polls in a row before resolving again
#define NTP_REFRESH_S        604800U // and once a week
#define SYNC_SLACK_S      300U
#define FREE_RUN_AFTER_S  86400U
#define PPS_SYNC_LIMIT_NS 1000000 // phase within which the edge marks its second
// The first poll on a link waits this long at most for SLAAC's global IPv6
// address, which follows IPv4 by a few seconds: started on IPv4, the
// frequency would be learnt on one path and continued on another.
#define IPV6_WAIT_MS 15000U
static_assert(ESP_CLOCK_YOUNG_SAMPLES == NTP_STATS_MIN_SAMPLES,
              "a young window fit and a young line are the same number of samples");

// The skew a young line's distance is ranked with (rank_distance_us): what
// a crystal's frequency, once estimated, is close enough to for choosing.
#define NTP_RANK_SKEW_PPB 5000
#define FED_BY_OTHER (-2)
// A server is usable while one of its last three polls was good ...
#define REACH_RECENT 0x07U

typedef struct
{
    char host[USER_PREFS_STORED_STRING_SIZE]; // as configured when last reset
    bool configured;
    int64_t next_poll_us;
    bool due;    // poll as soon as the link allows (start, link up, resync)
    bool resync; // a poll asked for (ntp_resync), not yet started
    uint32_t retry_s;
    uint32_t learn_step; // 64, 128, 256 s polls done
    uint8_t reach;       // a bit per poll, the latest lowest: 1 = good
    // The name's addresses, preferred family first, and the one in use: a
    // poll with no reply at all goes on to the next at once. Resolved once
    // and kept (see resolve_if_needed), so a pool or anycast name keeps its
    // address, and its statistics, from poll to poll.
    IPAddress targets[NET_ADDRESSES];
    int target_count;
    int target;
    int64_t resolved_us;   // when, 0 = never
    uint8_t failures;      // polls in a row with no good sample
    bool resolve;          // resolve again before the next poll
    uint32_t resolves;
    char address[48]; // the address last polled
    char stats_address[48]; // the address its statistics are of
    EspClockSample sample; // the latest good one
    uint32_t count;        // good samples since reset
    uint32_t restarts;     // its statistics started again: another address, or a jump
    int64_t measured_us;      // the latest sample against the clock when it came
    int64_t applied_local_us; // the latest sample given to the clock, if any
    NtpPollResult last;       // the latest poll, good or bad
    bool have_last;
    // From the latest selection: its line's offset now against the clock,
    // its root distance and the samples' spread about the line.
    int64_t offset_us;
    int64_t distance_us;
    int64_t jitter_us;
    NtpVerdict verdict;
    uint32_t polls;
    uint32_t good_polls;
    uint32_t fallbacks;
    uint32_t dns_failures;
    // 1 + the server it is the same address as (a lower number): not polled,
    // not selected or counted. 0 = none.
    uint8_t same_as;
    // Kiss-o'-death: its interval doubled rate_shift times (RATE), and
    // whether it has refused (DENY, RSTR); counts of each code.
    uint8_t rate_shift;
    uint8_t good_since_rate;
    bool refused;
    char refused_code[5];
    uint32_t kisses_rate;
    uint32_t kisses_refused;
    uint32_t kisses_other;
} NtpServer;

typedef struct
{
    NtpServer servers[NTP_SLOTS];
    int peer;    // the server the clock follows, -1 if none
    int polling; // the server with a poll in flight, -1 if none
    int fed_by;  // the server whose samples the frequency window holds: -1 none
                 // yet, FED_BY_OTHER one since changed
    NtpSelection selection;
    uint32_t combined_updates; // clock updates from more than one server
    uint8_t distant[NTP_SLOTS]; // ntp_combine()'s, chronyd's distant counts
    uint8_t combined_mask;        // the servers combined last, reported on change
    bool disagree;           // reported: the usable servers have no majority
    uint32_t max_interval_s; // ntp.interval: the longest poll interval
    uint32_t interval_s;     // the interval in use, adaptive below that
    double poll_score;       // chronyd's: the interval doubles at 1, halves below 0
    TempCompPolls short_polls; // what was asked for at the last pass (temp_comp.h)
    bool report_ready;
    SML_TIME_NTP_PAYLOAD report;
    uint32_t edges;
    int64_t last_edge_utc_us;
    bool polled_on_link; // a poll has been started since the network came up
    bool network_was_up;
    uint32_t network_up_ms; // when it came up
} Ntp;

static Ntp ntp;
// Each server's samples and fitted line, apart from the rest (they are
// large, and NtpServer is reset by assignment).
static NtpStats server_stats[NTP_SLOTS];

static int64_t now_us(void)
{
    return esp_timer_get_time();
}


// What a slot is set to: "dhcp" for server 0 unless disabled, else server
// index's name ("" if none).
static const char *slot_host(int index)
{
    if (index == DHCP_SLOT)
    {
        return user_prefs_ntp_dhcp != NTP_DHCP_DISABLED ? DHCP_HOST : "";
    }
    return user_prefs_ntp_server(index - 1);
}


static void reset_server(int index)
{
    NtpServer *server = &ntp.servers[index];

    *server = NtpServer(); // zeroed, with IPAddress constructed
    snprintf(server->host, sizeof(server->host), "%s", slot_host(index));
    server->configured = server->host[0] != '\0';
    server->due = true;
    server->retry_s = RETRY_FIRST_S;
    server->verdict = server->configured ? NTP_VERDICT_UNUSABLE : NTP_VERDICT_NONE;
    ntp_stats_reset(&server_stats[index]);
    ntp.distant[index] = 0;
    if (ntp.peer == index)
    {
        ntp.peer = -1;
    }
    if (ntp.fed_by == index)
    {
        ntp.fed_by = FED_BY_OTHER;
    }
}


void ntp_init(void)
{
    esp_clock_init();
    temp_comp_init();
    temp_comp_restore(user_prefs_esp_tcomp.learnt_ppb_per_c, user_prefs_esp_tcomp.learnt_estimates);
    ntp_client_init();
    ntp.peer = -1;
    ntp.polling = -1;
    ntp.fed_by = -1;
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        reset_server(index);
    }
    ntp.max_interval_s = user_prefs.fields.ntp.interval;
    ntp.interval_s = POLL_START_S < ntp.max_interval_s ? POLL_START_S : ntp.max_interval_s;
    ntp.short_polls = TEMP_COMP_POLLS_NORMAL;
}


// A poll of every server now: or, for one being polled, as soon as it ends,
// which would otherwise schedule the next for an interval on.
void ntp_resync(void)
{
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        ntp.servers[index].due = true;
        ntp.servers[index].resync = true;
    }
}


bool ntp_is_valid(void)
{
    return esp_clock_is_valid();
}


static uint32_t update_age_s(void)
{
    int64_t age = now_us() - esp_clock_status()->last_update_local_us;

    return age < 0 ? 0 : (uint32_t)(age / 1000000);
}


bool ntp_set_asymmetry(int32_t us)
{
    int64_t delta = (int64_t)us - user_prefs_ntp_asymmetry_us;

    if (us < -NTP_ASYMMETRY_MAX_US || us > NTP_ASYMMETRY_MAX_US)
    {
        return false;
    }
    user_prefs_ntp_asymmetry_us = us;
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        ntp_stats_shift(&server_stats[index], delta);
        ntp.servers[index].sample.offset_us += delta;
    }
    // The clock's own window holds the old samples: start it again. The
    // next update slews the clock by the change.
    esp_clock_new_reference();
    return true;
}


bool ntp_set_offset(int server, int32_t us)
{
    int64_t delta;

    if (server < 0 || server >= NTP_SLOTS || us < -NTP_OFFSET_MAX_US || us > NTP_OFFSET_MAX_US)
    {
        return false;
    }
    delta = (int64_t)us - user_prefs_ntp_offset_us[server];
    user_prefs_ntp_offset_us[server] = us;
    if (delta != 0)
    {
        ntp_stats_shift(&server_stats[server], delta);
        ntp.servers[server].sample.offset_us += delta;
        esp_clock_new_reference(); // as for ntp.asymmetry
    }
    return true;
}


uint8_t ntp_servers_in_use(void)
{
    return ntp.peer >= 0 ? ntp.combined_mask : 0;
}


bool ntp_is_synced(void)
{
    return esp_clock_is_valid() && update_age_s() <= 2U * ntp.max_interval_s + SYNC_SLACK_S;
}


bool ntp_servers_disagree(void)
{
    return ntp.disagree;
}


bool ntp_utc_at(int64_t local_us, int64_t *utc_us)
{
    if (!esp_clock_is_valid())
    {
        return false;
    }
    *utc_us = esp_clock_utc_us(local_us);
    return true;
}


bool ntp_utc_now(uint32_t *utc_seconds)
{
    int64_t utc;

    if (!ntp_utc_at(now_us(), &utc))
    {
        return false;
    }
    *utc_seconds = (uint32_t)(utc / 1000000);
    return true;
}


// The system clock follows the model, for anything using time() or TLS.
static void set_system_time(void)
{
    int64_t utc = esp_clock_utc_us(now_us());
    struct timeval value;

    value.tv_sec = (time_t)(utc / 1000000);
    value.tv_usec = (suseconds_t)(utc % 1000000);
    settimeofday(&value, NULL);
}


// The longest poll temp_comp asks for, within the adaptive range; 0: none.
static uint32_t short_polls_s(TempCompPolls polls)
{
    uint32_t cap = temp_comp_polls_s(polls);
    uint32_t shortest = POLL_MIN_S < ntp.max_interval_s ? POLL_MIN_S : ntp.max_interval_s;

    if (cap == 0)
    {
        return 0;
    }
    cap = cap < ntp.max_interval_s ? cap : ntp.max_interval_s;
    return cap > shortest ? cap : shortest;
}


/*
 * The poll interval adapts between POLL_MIN_S and ntp.interval as chronyd
 * adapts it (ntp_core.c), on each sample from the server followed:
 *
 *   - If the sample is further from the server's line than its own
 *     distance (half the round trip), the line is not tracking it:
 *     -log2(error / distance) is added to the score.
 *   - Else the score moves by (samples in the line / POLL_TARGET - 1) /
 *     POLL_TARGET, twice that when below the target. The runs test keeps
 *     the line short while the crystal wanders, so the polls shorten; a
 *     steady crystal lets it grow, and they lengthen.
 *
 * A score of 1 doubles the interval, one below 0 halves it. Every server
 * is polled at the interval.
 */
static void adapt_interval(int64_t error_us, int64_t distance_us, unsigned samples)
{
    uint32_t longest = ntp.max_interval_s;
    uint32_t shortest = POLL_MIN_S < longest ? POLL_MIN_S : longest;

    // While the temperature compensation starts out, or the temperature
    // moves: shorter polls (temp_comp.h).
    uint32_t cap = short_polls_s(temp_comp_short_polls(now_us()));

    if (cap != 0)
    {
        longest = cap;
    }
    uint32_t interval;
    double adjust;

    if (error_us < 0)
    {
        error_us = -error_us;
    }
    if (distance_us < 1)
    {
        distance_us = 1;
    }
    if (error_us > distance_us)
    {
        adjust = -log2((double)error_us / (double)distance_us);
    }
    else
    {
        adjust = ((double)samples / POLL_TARGET - 1.0) / POLL_TARGET;
        if (samples < POLL_TARGET)
        {
            adjust *= 2.0;
        }
    }
    ntp.poll_score += adjust;
    interval = ntp.interval_s;
    while (ntp.poll_score >= 1.0)
    {
        interval *= 2;
        ntp.poll_score -= 1.0;
    }
    while (ntp.poll_score < 0.0)
    {
        interval /= 2;
        ntp.poll_score += 1.0;
    }
    // Held at the ends as chronyd holds them: at the shortest, the score
    // from nothing; at the longest, one step from lengthening.
    if (interval < shortest)
    {
        interval = shortest;
        ntp.poll_score = 0.0;
    }
    else if (interval > longest)
    {
        interval = longest;
        ntp.poll_score = 1.0;
    }
    ntp.interval_s = interval;
}


// The interval for this server: ntp.interval_s, doubled for each RATE kiss
// still in force, at most RATE_MAX_S.
static uint32_t server_interval_s(const NtpServer *server)
{
    uint64_t interval = (uint64_t)ntp.interval_s << server->rate_shift;

    return interval > RATE_MAX_S ? (ntp.interval_s > RATE_MAX_S ? ntp.interval_s : RATE_MAX_S)
                                 : (uint32_t)interval;
}


static uint32_t next_delay_s(NtpServer *server, bool good)
{
    uint32_t delay_s;

    if (!good)
    {
        delay_s = server->retry_s;
        server->retry_s =
            server->retry_s * 2 > ntp.interval_s ? ntp.interval_s : server->retry_s * 2;
        return delay_s;
    }
    server->retry_s = RETRY_FIRST_S;
    if (esp_clock_status()->frequency_samples < 2 && server->learn_step < 3 &&
        server->rate_shift == 0)
    {
        delay_s = LEARN_FIRST_S << server->learn_step++;
        return delay_s < ntp.interval_s ? delay_s : ntp.interval_s;
    }
    return server_interval_s(server);
}


static void schedule_next(NtpServer *server, bool good)
{
    uint32_t delay_s = next_delay_s(server, good); // keeps its own count either way

    server->next_poll_us = now_us() + (int64_t)delay_s * 1000000;
    server->due = server->resync;
}


static void start_pps(void)
{
    int64_t utc = esp_clock_utc_us(now_us());
    int64_t next_second = (utc / 1000000 + 1) * 1000000;

    pps_output_start(esp_clock_local_us(next_second), esp_clock_period_adjust_q16());
}


// The server as named, and the address it was polled at unless that is
// its name: "pool.example (192.0.2.10)", "192.0.2.10".
#define SERVER_TEXT (USER_PREFS_STORED_STRING_SIZE + 52)
static const char *server_text(const NtpServer *server, char *text)
{
    if (server->address[0] == '\0' || strcasecmp(server->address, server->host) == 0)
    {
        snprintf(text, SERVER_TEXT, "%s", server->host);
    }
    else
    {
        snprintf(text, SERVER_TEXT, "%s (%s)", server->host, server->address);
    }
    return text;
}


static const char *poll_failure(const NtpPollResult *poll)
{
    if (poll->dns_failed)
    {
        return "DNS lookup failed";
    }
    if (poll->last_result == NTP_REPLY_OK)
    {
        return "no reply";
    }
    return ntp_reply_name(poll->last_result);
}


// ---- Samples and selection -----------------------------------------------------

/*
 * An event to the console, led by the NTP clock's UTC time of day
 * ("12:34:56Z NTP: following ..."), so a session log can be lined up with
 * "ntp gnss -v"; "--:--:--Z" before the clock is set.
 */
static void ntp_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void ntp_event(const char *format, ...)
{
    char text[160];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (esp_clock_is_valid())
    {
        uint32_t day_s = (uint32_t)((uint64_t)(esp_clock_utc_us(now_us()) / 1000000) % 86400U);

        console_printf("%02u:%02u:%02uZ NTP: %s", (unsigned)(day_s / 3600U),
                       (unsigned)(day_s / 60U % 60U), (unsigned)(day_s % 60U), text);
    }
    else
    {
        console_printf("--:--:--Z NTP: %s", text);
    }
}


static const EspClockSample *latest(const NtpServer *server)
{
    return &server->sample;
}


// A sample's offset against the clock as it now stands: the server's view
// of our error at the time it was taken.
static int64_t against_clock(const EspClockSample *sample)
{
    return sample->local_us + sample->offset_us - esp_clock_model_utc_us(sample->local_us);
}


// The server's line (ntp_stats.h) run on to now, against the clock: its
// view of our error now.
static int64_t line_against_clock(int index, int64_t now)
{
    return now + ntp_stats_offset_at(&server_stats[index], now) - esp_clock_model_utc_us(now);
}


/*
 * Root distance, as chronyd has it for selection: half the round trip and
 * half the server's own root delay (the path's asymmetry could be all of
 * it) and its root dispersion, of the sample that gives the least, the
 * skew of the server's line over the time since that sample; and, as NTP
 * adds its jitter, the samples' spread about the line.
 */
static int64_t root_distance_us(int index, int64_t now)
{
    const NtpStats *stats = &server_stats[index];
    int64_t distance = ntp_stats_distance_us(stats, now) + (int64_t)llround(stats->sd_us);

    return distance > 0 ? distance : 1;
}


/*
 * What the followed server is chosen by: its root distance, but for a young
 * line (fewer than NTP_STATS_MIN_SAMPLES) with the skew taken as at most
 * NTP_RANK_SKEW_PPB. A line of 3-5 samples has a skew of hundreds or
 * thousands of ppm (Student's t at 1-3 degrees of freedom), so its distance is mostly skew x time since its last
 * poll, and whichever server was polled last looked closest: at power-on an
 * Internet server 1.3 ms off was followed over the LAN one for minutes,
 * and the clock took half an hour to come back. Capped, the choice goes by
 * the round trip, the server's own error and the jitter, which are known
 * from the first samples. The distance itself (agreement, combining) is
 * not changed.
 */
static int64_t rank_distance_us(int index, int64_t now)
{
    const NtpStats *stats = &server_stats[index];
    int64_t distance;

    if (stats->samples >= NTP_STATS_MIN_SAMPLES)
    {
        return 0; // the root distance
    }
    distance = ntp_stats_distance_capped_us(stats, now, NTP_RANK_SKEW_PPB) +
               (int64_t)llround(stats->sd_us);
    return distance > 0 ? distance : 1;
}


static bool reachable(int index, int64_t now)
{
    const NtpServer *server = &ntp.servers[index];
    int64_t limit_us = (2LL * ntp.max_interval_s + SYNC_SLACK_S) * 1000000;

    return server->configured && server->count != 0 && (server->reach & REACH_RECENT) != 0 &&
           now - latest(server)->local_us <= limit_us;
}


/*
 * A server can be selected once it has a line (three samples), as chronyd
 * selects a source only once its regression is good: before that its
 * distance is the worst case (2000 ppm of skew), growing 2 ms a second, so
 * whichever had answered last looked best and the clock hopped between
 * them for the first few minutes. Until any server has a line, the one
 * that set the clock is kept (select_server).
 */
static bool usable(int index, int64_t now)
{
    return reachable(index, now) && server_stats[index].fitted;
}


/*
 * The server's latest sample to the clock, unless it has had it, with the
 * estimate to steer by: the servers' combined offset now (ntp_combine(),
 * each server's line run on to now) and their combined frequency once
 * there is a line. The first sample of all sets the clock by itself.
 */
static void apply(int index)
{
    NtpServer *server = &ntp.servers[index];
    const EspClockSample *sample = latest(server);
    const EspClockStatus *status = esp_clock_status();
    EspClockEstimate estimate;
    EspClockAction action;
    int64_t now = now_us();
    bool estimated = esp_clock_is_valid() && ntp.selection.selected == index;

    if (server->count == 0 || server->applied_local_us == sample->local_us)
    {
        return;
    }
    if (estimated)
    {
        // The combined offset is our error now: UTC - local is that plus
        // the model's.
        estimate.offset_us = ntp.selection.combined_offset_us + esp_clock_model_utc_us(now) - now;
        estimate.frequency_known = ntp.selection.frequency_known;
        estimate.frequency_ppb = (int32_t)llround(ntp.selection.combined_frequency_ppb);
        estimate.skew_ppb = (int32_t)(ntp.selection.combined_skew_ppb < 1e9
                                          ? llround(ntp.selection.combined_skew_ppb)
                                          : 1000000000LL);
        if (ntp.selection.combined > 1)
        {
            ntp.combined_updates++;
        }
    }
    if (ntp.fed_by != index)
    {
        if (ntp.fed_by != -1)
        {
            esp_clock_new_reference();
        }
        ntp.fed_by = index;
    }
    server->applied_local_us = sample->local_us;
    // A gentle correction is spread over about two polls: the next sample
    // replaces what is left of it.
    action = esp_clock_update(sample, now, (int64_t)ntp.interval_s * 2000000,
                              estimated ? &estimate : NULL);
    set_system_time();
    if (action == ESP_CLOCK_STEPPED)
    {
        char step[TIME_NS_TEXT];
        char name[SERVER_TEXT];

        if (status->steps == 1)
        {
            // The first: nothing before it to have stepped from.
            ntp_event("clock set from %s\n", server_text(server, name));
        }
        else
        {
            time_format_ns(step, status->last_correction_us * 1000, true);
            ntp_event("clock stepped by %s from %s (%u steps)\n", step,
                           server_text(server, name), (unsigned)status->steps);
        }
        start_pps();
    }
    else if (action == ESP_CLOCK_SLEWED_FAST)
    {
        char slew[TIME_NS_TEXT];

        time_format_ns(slew, status->slew_us * 1000, true);
        ntp_event("correcting %s at %d ppm\n", slew, ESP_CLOCK_FAST_SLEW_PPM);
    }
}


/*
 * Chooses the server to follow (ntp_select.h) after any poll, and gives the
 * clock that server's latest sample if it has not had it: fresh is the
 * server just polled. The first good sample of all sets the clock, there
 * being nothing to judge it against.
 */
static void note_combined(void);


static void select_server(int fresh)
{
    NtpCandidate candidates[NTP_SLOTS];
    int64_t now = now_us();
    int previous = ntp.peer;
    bool disagree;

    if (!esp_clock_is_valid())
    {
        if (fresh < 0 || ntp.servers[fresh].count == 0)
        {
            return;
        }
        ntp.peer = fresh;
        apply(fresh);
        previous = fresh;
    }
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        NtpServer *server = &ntp.servers[index];

        const NtpStats *stats = &server_stats[index];

        candidates[index].configured = server->configured && server->same_as == 0;
        candidates[index].usable = server->same_as == 0 && usable(index, now);
        if (server->count != 0)
        {
            server->jitter_us = (int64_t)llround(stats->sd_us);
            server->offset_us = line_against_clock(index, now);
            server->distance_us = root_distance_us(index, now);
        }
        candidates[index].offset_us = server->offset_us;
        candidates[index].distance_us = server->distance_us;
        candidates[index].rank_us = server->count != 0 ? rank_distance_us(index, now) : 0;
        candidates[index].fitted = server->count != 0 && stats->fitted;
        candidates[index].frequency_ppb = stats->frequency_ppb;
        candidates[index].frequency_sd_ppb = stats->frequency_sd_ppb;
        candidates[index].skew_ppb = stats->skew_ppb;
        candidates[index].young = stats->samples < NTP_STATS_MIN_SAMPLES;
    }
    ntp_select(candidates, NTP_SLOTS, ntp.peer, &ntp.selection);
    if (ntp.selection.usable == 0 && previous >= 0 && reachable(previous, now))
    {
        // None has a line yet: stay with the server followed (the one that
        // set the clock), steering by its samples alone.
        ntp.selection.selected = previous;
        ntp.selection.verdict[previous] = NTP_VERDICT_SELECTED;
    }
    if (user_prefs_ntp_dhcp == NTP_DHCP_PREFERRED &&
        (ntp.selection.verdict[DHCP_SLOT] == NTP_VERDICT_CANDIDATE ||
         ntp.selection.verdict[DHCP_SLOT] == NTP_VERDICT_SELECTED))
    {
        // DHCP's server preferred, as chronyd's prefer: followed while it
        // agrees with the majority, and the others not combined with it
        // (marked young for ntp_combine: left out, gaining no distant count).
        if (ntp.selection.selected >= 0 && ntp.selection.selected != DHCP_SLOT)
        {
            ntp.selection.verdict[ntp.selection.selected] = NTP_VERDICT_CANDIDATE;
        }
        ntp.selection.selected = DHCP_SLOT;
        ntp.selection.verdict[DHCP_SLOT] = NTP_VERDICT_SELECTED;
        for (int index = 0; index < NTP_SLOTS; index++)
        {
            candidates[index].young = candidates[index].young || index != DHCP_SLOT;
        }
    }
    ntp_combine(candidates, NTP_SLOTS, ntp.distant, &ntp.selection);
    note_combined();
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        ntp.servers[index].verdict = ntp.selection.verdict[index];
    }

    ntp.peer = ntp.selection.selected;
    if (ntp.peer >= 0 && ntp.peer != previous)
    {
        char name[SERVER_TEXT];

        ntp_event("following %s\n", server_text(&ntp.servers[ntp.peer], name));
    }
    else if (ntp.peer < 0 && previous >= 0)
    {
        ntp_event("no server usable, coasting\n");
    }
    if (ntp.peer >= 0)
    {
        apply(ntp.peer);
    }

    disagree = !ntp.selection.majority && ntp.selection.usable >= 2;
    if (disagree != ntp.disagree)
    {
        ntp.disagree = disagree;
        ntp_event(disagree ? "servers DISAGREE, no majority\n" : "servers agree\n");
    }
}


// "NTP: combining 1, 2" (server numbers) when the set combined changes,
// "NTP: combining 2 alone" when it falls to the followed server.
static void note_combined(void)
{
    uint8_t mask = 0;

    for (int index = 0; index < NTP_SLOTS; index++)
    {
        if (ntp.selection.in_combined[index])
        {
            mask = (uint8_t)(mask | (1U << index));
        }
    }
    if (mask == ntp.combined_mask)
    {
        return;
    }
    ntp.combined_mask = mask;
    if (mask == 0)
    {
        return;
    }
    if (ntp.selection.combined == 1)
    {
        ntp_event("combining %d alone\n", ntp.selection.selected);
        return;
    }
    {
        char list[4 * NTP_SLOTS] = "";
        size_t used = 0;

        for (int index = 0; index < NTP_SLOTS; index++)
        {
            if (mask & (1U << index))
            {
                used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%d",
                                         used != 0 ? ", " : "", index);
            }
        }
        ntp_event("combining %s\n", list);
    }
}


/*
 * A good sample to the server's statistics. They start again for another
 * address (another server behind the name, another path), or a sample off
 * the server's line by more than a step (the server's time jumped): the
 * old samples would hold the line, and the clock, where it was.
 */
// True with *off_line_us, the sample against the line before it, if it
// joined a line.
static bool add_to_stats(int index, const EspClockSample *sample, int64_t *off_line_us)
{
    NtpServer *server = &ntp.servers[index];
    NtpStats *stats = &server_stats[index];
    NtpStatsSample entry;
    bool joined = false;

    if (stats->samples != 0)
    {
        *off_line_us = sample->offset_us - ntp_stats_offset_at(stats, sample->local_us);
        joined = stats->fitted;
        if (strcmp(server->stats_address, server->address) != 0 ||
            *off_line_us > ESP_CLOCK_STEP_US || *off_line_us < -ESP_CLOCK_STEP_US)
        {
            ntp_stats_reset(stats);
            server->restarts++;
            joined = false;
            if (ntp.fed_by == index)
            {
                // Another address, or a jump: the clock's window holds the
                // old path's samples, so it starts again at the next one
                // given to it, as when the server followed changes.
                ntp.fed_by = FED_BY_OTHER;
            }
        }
    }
    static_assert(sizeof(server->stats_address) == sizeof(server->address), "same size");
    memcpy(server->stats_address, server->address, sizeof(server->stats_address));
    entry.local_us = sample->local_us;
    entry.offset_us = sample->offset_us;
    entry.delay_us = sample->delay_us < 0 ? 0U : (uint32_t)sample->delay_us;
    entry.server_us = sample->root_delay_us / 2U + sample->root_dispersion_us;
    // With the crystal compensated, it moves little in half an hour: no
    // shorter line, which a few polls over a shifted path would tilt.
    stats->min_span_s = temp_comp_status()->state == TEMP_COMP_ACTIVE ? NTP_STATS_MIN_SPAN_S : 0.0f;
    ntp_stats_add(stats, &entry);
    return joined;
}


/*
 * A kiss-o'-death (stratum 0; its origin already matched our request, so
 * not spoofed off-path), as RFC 5905 7.4 asks. True if handled here (RATE,
 * DENY, RSTR): the poll is then scheduled by its code, not as a failure's
 * retry. Other codes are failures as before.
 */
static bool kissed(NtpServer *server, const char *code)
{
    if (strcmp(code, "RATE") == 0)
    {
        server->kisses_rate++;
        if (((uint64_t)ntp.interval_s << (server->rate_shift + 1)) <= RATE_MAX_S)
        {
            server->rate_shift++;
        }
        server->good_since_rate = 0;
        server->next_poll_us = now_us() + (int64_t)server_interval_s(server) * 1000000;
        server->due = false;
        ntp_event("%s asks to be polled less often (RATE): every %lu s\n", server->host,
                  (unsigned long)server_interval_s(server));
        return true;
    }
    if (strcmp(code, "DENY") == 0 || strcmp(code, "RSTR") == 0)
    {
        server->kisses_refused++;
        server->refused = true;
        snprintf(server->refused_code, sizeof(server->refused_code), "%s", code);
        server->next_poll_us = now_us() + (int64_t)NTP_REFRESH_S * 1000000;
        server->due = false;
        ntp_event("%s refuses to serve (%s): not polled again for a week, or until its "
                  "setting changes, the network returns or ntp sync\n",
                  server->host, code);
        return true;
    }
    server->kisses_other++;
    return false;
}


static void handle_poll(int index, const NtpPollResult *poll)
{
    NtpServer *server = &ntp.servers[index];
    int64_t off_line_us = 0;
    EspClockSample sample;
    bool joined;

    server->last = *poll;
    server->have_last = true;
    server->polls++;
    server->reach = (uint8_t)(server->reach << 1 | (poll->ok ? 1U : 0U));
    // A kiss is the server answering, not the path failing: it does not
    // count towards looking the name up again.
    if (!poll->ok && poll->kiss[0] == '\0' && server->failures < UINT8_MAX)
    {
        server->failures++;
    }
    if (poll->ok)
    {
        server->failures = 0;
    }
    if (!poll->ok)
    {
        if (poll->kiss[0] != '\0' && kissed(server, poll->kiss))
        {
            select_server(-1);
            return;
        }
        ntp_event("poll of %s failed (%s %s)\n", server->host, poll_failure(poll),
                       poll->kiss);
        schedule_next(server, false);
        select_server(-1);
        return;
    }
    server->good_polls++;
    server->refused = false;
    if (server->rate_shift != 0 && ++server->good_since_rate >= RATE_EASE_POLLS)
    {
        // Answered at the slower rate for a while: try the next faster one.
        server->rate_shift--;
        server->good_since_rate = 0;
    }
    // ntp.asymmetry: the ESP's own end of every path; ntp offset: this
    // server's path.
    sample = poll->sample;
    sample.offset_us += user_prefs_ntp_asymmetry_us + user_prefs_ntp_offset_us[index];
    if (index == ntp.peer)
    {
        temp_comp_learn(sample.local_us, sample.offset_us, sample.delay_us, index);
    }
    // The temperature compensation taken off: the lines and the model see
    // what it does not explain (temp_comp.h).
    sample.offset_us -= temp_comp_us(sample.local_us);
    server->measured_us = esp_clock_is_valid() ? against_clock(&sample) : 0;
    server->sample = sample;
    server->count++;
    joined = add_to_stats(index, &sample, &off_line_us);
    select_server(index);
    if (joined && index == ntp.peer && esp_clock_status()->frequency_samples >= 2)
    {
        adapt_interval(off_line_us, sample.delay_us / 2 + (int64_t)NTP_STATS_PRECISION_US,
                       server_stats[index].samples);
    }
    schedule_next(server, true);
}


// ---- TIME-NTP and the ESP PPS ---------------------------------------------------

/*
 * The clock's bound at local_us, ns (esp_clock_uncertainty_ns()); UINT32_MAX
 * when it is not bounded within 4.29 s (TIME-NTP then clears
 * UNCERTAINTY_VALID, rather than give a smaller bound than the truth).
 *
 * While the usable servers disagree (no majority), the truth is in one of
 * their intervals but which is not known: the clock may be as far off as
 * the furthest edge of any of them. The one followed is kept (no coasting),
 * but the bound says so. Worked out at local_us, from each server's line
 * run on to then and against the clock then, so a slew under way since
 * the last selection is in it (a bound taken at selection and carried on
 * understated it by however much had been slewed since).
 */
static uint32_t ntp_uncertainty_ns(int64_t local_us)
{
    uint32_t clock_ns = esp_clock_uncertainty_ns(local_us);
    int64_t ns = clock_ns;

    if (clock_ns == UINT32_MAX)
    {
        return UINT32_MAX;
    }
    if (ntp.disagree)
    {
        for (int index = 0; index < NTP_SLOTS; index++)
        {
            int64_t edge_us;

            if (ntp.servers[index].same_as != 0 || ntp.servers[index].count == 0 ||
                !usable(index, local_us))
            {
                continue;
            }
            edge_us = line_against_clock(index, local_us);
            edge_us = (edge_us < 0 ? -edge_us : edge_us) + root_distance_us(index, local_us);
            ns = edge_us * 1000 > ns ? edge_us * 1000 : ns;
        }
    }
    return ns >= (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ns;
}


static void build_report(const PpsEdge *edge)
{
    SML_TIME_NTP_PAYLOAD *report = &ntp.report;
    const EspClockStatus *status = esp_clock_status();
    int64_t utc = esp_clock_utc_us(edge->local_us);
    int64_t second = (utc + 500000) / 1000000;
    int64_t phase_ns = (utc - second * 1000000) * 1000;
    uint32_t age_s = update_age_s();
    int64_t age_ms = (now_us() - status->last_update_local_us) / 1000;

    memset(report, 0, sizeof(*report));
    report->flags = SML_NTP_TIME_VALID | SML_NTP_UNCERTAINTY_VALID;
    if (status->updates != 0)
    {
        report->flags |= SML_NTP_UPDATE_VALID;
    }
    if (phase_ns < PPS_SYNC_LIMIT_NS && phase_ns > -PPS_SYNC_LIMIT_NS)
    {
        report->flags |= SML_NTP_PPS_SYNC | SML_NTP_SCHEDULER_SYNC;
    }
    if (esp_clock_correcting(edge->local_us))
    {
        report->flags |= SML_NTP_CORRECTING;
    }
    report->source = ntp_is_synced() ? SML_SOURCE_NTP : SML_SOURCE_ESP;
    if (ntp_is_synced())
    {
        // LOCKED once the server followed has a full line (its frequency is
        // known, not guessed from two or three samples).
        bool mature = ntp.peer >= 0 ? server_stats[ntp.peer].samples >= NTP_STATS_MIN_SAMPLES
                                    : status->frequency_samples != 0;

        report->lock_state = status->frequency_samples != 0 && mature ? SML_LOCK_LOCKED
                                                                      : SML_LOCK_ACQUIRING;
    }
    else
    {
        report->lock_state = age_s > FREE_RUN_AFTER_S ? SML_LOCK_FREE_RUNNING : SML_LOCK_HOLDOVER;
    }
    report->utc_seconds = (uint32_t)second;
    report->utc_phase_ns = (int32_t)phase_ns;
    report->uncertainty_ns = ntp_uncertainty_ns(edge->local_us);
    if (report->uncertainty_ns == UINT32_MAX)
    {
        report->flags &= (uint16_t)~SML_NTP_UNCERTAINTY_VALID; // not bounded within 4.29 s
    }
    report->pps_sequence = edge->sequence;
    report->last_update_utc = (uint32_t)(status->last_update_utc_us / 1000000);
    report->update_age_ms = age_ms > 65535 ? 65535 : (uint16_t)(age_ms < 0 ? 0 : age_ms);
    report->update_interval_s = ntp.interval_s;
    report->round_trip_us = (uint32_t)status->last.delay_us;
    report->last_offset_us = (int32_t)status->last_error_us;
    // Positive: the ESP's clock runs fast.
    report->frequency_estimate_ppt = -status->frequency_ppb * 1000;
    ntp.report_ready = true;
    ntp.last_edge_utc_us = utc;
}


static void service_pps(void)
{
    PpsEdge edge;

    if (!pps_output_take(&edge) || !esp_clock_is_valid())
    {
        return;
    }
    ntp.edges++;
    build_report(&edge);
    // The next edge: the next whole UTC second after this one.
    pps_output_plan(esp_clock_local_us(((int64_t)ntp.report.utc_seconds + 1) * 1000000),
                    esp_clock_period_adjust_q16());
}


// ---- Polling --------------------------------------------------------------------

// A poll of server index at address, by the client task.
static void start_poll(int index, const IPAddress *address)
{
    NtpServer *server = &ntp.servers[index];
    NtpServerAddress target;
    IPAddress copy = *address;

    memset(&target, 0, sizeof(target));
    target.ipv6 = copy.type() == IPv6;
    memcpy(target.bytes, &copy[0], target.ipv6 ? 16 : 4);
    target.zone = copy.zone();
    snprintf(server->address, sizeof(server->address), "%s", copy.toString().c_str());
    if (ntp_client_poll(&target))
    {
        ntp.polling = index;
    }
}


/*
 * The interval cut from old_s to new_s (ntp.interval lowered, or short
 * polls asked for): polls already scheduled under the old one would
 * otherwise stand (a day, say, after going to 128 s). Each one still to
 * come is brought in by the ratio of the two, so they keep their order and
 * spread rather than all falling due at once, and none is left further off
 * than its interval now; retries' back-off is capped at it.
 */
static void pull_polls_forward(uint32_t old_s, uint32_t new_s)
{
    int64_t now = now_us();

    for (int index = 0; index < NTP_SLOTS; index++)
    {
        NtpServer *server = &ntp.servers[index];
        int64_t limit_us = (int64_t)server_interval_s(server) * 1000000;
        int64_t left_us = server->next_poll_us - now;

        if (server->retry_s > ntp.interval_s)
        {
            server->retry_s = ntp.interval_s;
        }
        if (server->refused || left_us <= 0)
        {
            continue; // a refusal's week stands; overdue ones are due anyway
        }
        left_us = left_us * (int64_t)new_s / (int64_t)old_s;
        server->next_poll_us = now + (left_us < limit_us ? left_us : limit_us);
    }
}


// A server's setting changed (ntp server, defaults): it starts again.
static void check_settings(void)
{
    if (ntp.max_interval_s != user_prefs.fields.ntp.interval)
    {
        uint32_t old_max_s = ntp.max_interval_s;

        ntp.max_interval_s = user_prefs.fields.ntp.interval;
        if (ntp.interval_s > ntp.max_interval_s)
        {
            ntp.interval_s = ntp.max_interval_s;
        }
        if (ntp.max_interval_s < old_max_s)
        {
            pull_polls_forward(old_max_s, ntp.max_interval_s);
        }
    }
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        if (index != ntp.polling &&
            strcmp(ntp.servers[index].host, slot_host(index)) != 0)
        {
            reset_server(index);
            select_server(-1);
        }
    }
}


static void take_result(bool network_up)
{
    NtpPollResult poll;
    NtpServer *server;

    if (ntp.polling < 0 || !ntp_client_take(&poll))
    {
        return;
    }
    server = &ntp.servers[ntp.polling];
    if (strcmp(server->host, slot_host(ntp.polling)) != 0)
    {
        ntp.polling = -1; // changed while it was polled: check_settings resets it
        return;
    }
    // No reply at all, rather than a refusal (a kiss) or a bad reply: the
    // address family may not route. The other, if there is one.
    bool silent = !poll.ok && poll.kiss[0] == '\0' && poll.last_result == NTP_REPLY_OK;
    int index = ntp.polling;

    ntp.polling = -1;
    net_note_result(server->targets[server->target], !silent);
    if (silent && server->target + 1 < server->target_count && network_up)
    {
        server->target++;
        server->fallbacks++;
        ntp_event("no reply from %s, trying %s\n", poll.address,
                       server->targets[server->target].toString().c_str());
        start_poll(index, &server->targets[server->target]);
        return;
    }
    handle_poll(index, &poll);
}


/*
 * The server's addresses, resolved here in the loop task (ntp_client.h):
 * both families, the preferred first (net_connect.h). Resolved once and
 * kept, as chronyd keeps a source's address: a pool or anycast name gives
 * another address almost every time, and each new address restarts the
 * server's statistics, so a server resolved at every poll never had a line
 * (uk.pool.ntp.org restarted 22 times in 23 polls). Resolved again only:
 *
 *   - with none yet, or after the network came back;
 *   - after NTP_RESOLVE_FAILURES polls in a row without a good sample (the
 *     address may have gone), taking the first address the name now gives;
 *   - once every NTP_REFRESH_S, a week (chronyd's refresh is two weeks), staying
 *     on the address in use if the name still gives it.
 *
 * If the name will not resolve but addresses are kept, they are polled.
 * False only with no address at all.
 */
static bool resolve_if_needed(int index)
{
    NtpServer *server = &ntp.servers[index];
    IPAddress fresh[NET_ADDRESSES];
    IPAddress current;
    int64_t now = now_us();
    bool have = server->target_count > 0;
    bool failing = server->failures >= NTP_RESOLVE_FAILURES;
    bool stale = now - server->resolved_us >= (int64_t)NTP_REFRESH_S * 1000000;
    int count;

    if (index == DHCP_SLOT)
    {
        // DHCP's server: no lookup, but the latest lease's, read each poll
        // (a new lease may offer another; its statistics then restart).
        IPAddress offered;

        if (!dhcp_ntp_server(&offered))
        {
            return have;
        }
        if (!have || !(server->targets[0] == offered))
        {
            server->targets[0] = offered;
            server->target_count = 1;
            server->target = 0;
            server->resolves++;
            server->resolved_us = now;
        }
        return true;
    }
    if (have && !server->resolve && !failing && !stale)
    {
        return true;
    }
    if (have)
    {
        current = server->targets[server->target];
    }
    count = net_resolve(server->host, fresh);
    server->resolve = false;
    if (count == 0)
    {
        server->dns_failures++;
        return have; // the kept addresses, for now
    }
    server->resolves++;
    server->resolved_us = now;
    server->failures = failing ? 0 : server->failures;
    server->target_count = count;
    server->target = 0;
    for (int i = 0; i < count; i++)
    {
        server->targets[i] = fresh[i];
        if (have && !failing && fresh[i] == current)
        {
            server->target = i; // still given: stay on it
        }
    }
    return true;
}


/*
 * A lower-numbered server polled at the same address (another name for it,
 * DHCP's offering a server also set by name, a pool handing out one already
 * listed), or -1. One server under two numbers would be combined at double
 * weight and counted twice towards a majority, so a wrong server set twice
 * could outvote a right one. chronyd refuses a second source at an address
 * it has. The lower number is kept (DHCP's 0 first).
 */
static int same_address_as(int index, const IPAddress *address)
{
    for (int other = 0; other < index; other++)
    {
        const NtpServer *server = &ntp.servers[other];

        if (server->configured && server->same_as == 0 && server->target_count > 0 &&
            server->targets[server->target] == *address)
        {
            return other;
        }
    }
    return -1;
}


// Server index set aside as the same address as server other: its samples
// and line dropped (they are other's), not polled until that changes.
static void set_aside(int index, int other)
{
    NtpServer *server = &ntp.servers[index];
    char name[SERVER_TEXT];

    if (server->same_as == other + 1)
    {
        return;
    }
    server->same_as = (uint8_t)(other + 1);
    snprintf(server->address, sizeof(server->address), "%s",
             server->targets[server->target].toString().c_str());
    server->count = 0;
    server->reach = 0;
    server->verdict = NTP_VERDICT_NONE;
    ntp_stats_reset(&server_stats[index]);
    ntp.distant[index] = 0;
    if (ntp.peer == index)
    {
        ntp.peer = -1;
    }
    if (ntp.fed_by == index)
    {
        ntp.fed_by = FED_BY_OTHER;
    }
    ntp_event("%s is the same address as server %d: not polled\n", server_text(server, name), other);
    if (user_prefs_ntp_offset_us[index] != user_prefs_ntp_offset_us[other])
    {
        // One path, two corrections: server other's is the one used. Not
        // carried over, so which applies never depends on what DHCP offers.
        ntp_event("server %d's offset %+ld us is not used: server %d's (%+ld us) is\n", index,
                  (long)user_prefs_ntp_offset_us[index], other, (long)user_prefs_ntp_offset_us[other]);
    }
}


// The server to poll next: due, the longest overdue first.
static int next_due(void)
{
    int64_t now = now_us();
    int chosen = -1;

    for (int index = 0; index < NTP_SLOTS; index++)
    {
        const NtpServer *server = &ntp.servers[index];

        if (!server->configured || (!server->due && now < server->next_poll_us))
        {
            continue;
        }
        if (index == DHCP_SLOT && server->target_count == 0)
        {
            IPAddress offered;

            if (!dhcp_ntp_server(&offered))
            {
                continue; // none offered (yet): nothing to poll
            }
        }
        if (chosen < 0 || (server->due && !ntp.servers[chosen].due) ||
            (server->due == ntp.servers[chosen].due &&
             server->next_poll_us < ntp.servers[chosen].next_poll_us))
        {
            chosen = index;
        }
    }
    return chosen;
}


// Short polls asked for (temp_comp.h): in at once, not from the next
// adaptation, which could be an interval away. Their end needs nothing:
// the interval lengthens from the next sample by the usual rule.
static void short_polls_now(void)
{
    TempCompPolls polls = temp_comp_short_polls(now_us());
    uint32_t cap = short_polls_s(polls);

    if (polls != ntp.short_polls)
    {
        if (polls != TEMP_COMP_POLLS_NORMAL)
        {
            ntp_event("short polls: %s, %lu s\n", temp_comp_polls_name(polls), (unsigned long)cap);
        }
        else
        {
            ntp_event("short polls end\n");
        }
        ntp.short_polls = polls;
    }
    if (cap != 0 && ntp.interval_s > cap)
    {
        uint32_t old_s = ntp.interval_s;

        ntp.interval_s = cap;
        ntp.poll_score = 0.0;
        pull_polls_forward(old_s, cap);
    }
}


// What the temperature compensation learns, into NVS: the first at once,
// then at most hourly (flash wear), the latest then.
#define TCOMP_SAVE_US (3600LL * 1000000)

static void keep_learnt(int64_t now)
{
    static int64_t saved_us;
    static bool pending;
    int32_t ppb_per_c, estimates;

    if (temp_comp_take_learnt(&ppb_per_c, &estimates))
    {
        user_prefs_esp_tcomp.learnt_ppb_per_c = ppb_per_c;
        user_prefs_esp_tcomp.learnt_estimates = estimates;
        ntp_event("crystal %+ld ppb/C learnt (%ld estimates)\n", (long)ppb_per_c,
                  (long)estimates);
        pending = true;
        if (estimates == 1)
        {
            saved_us = now - TCOMP_SAVE_US;
        }
    }
    if (pending && now - saved_us >= TCOMP_SAVE_US)
    {
        user_prefs_save_tcomp_learnt();
        saved_us = now;
        pending = false;
    }
}


void ntp_board_temperature(int32_t centic)
{
    temp_comp_temperature(now_us(), centic);
}


void ntp_service(bool network_up)
{
    NtpServer *server;
    int index;
    int64_t now = now_us();

    temp_comp_configure(now, user_prefs_esp_tcomp.enabled != 0, user_prefs_esp_tcomp.set_ppb_per_c);
    temp_comp_service(now);
    keep_learnt(now);
    short_polls_now();
    check_settings();
    take_result(network_up);
    service_pps();

    if (!network_up)
    {
        for (index = 0; index < NTP_SLOTS; index++)
        {
            ntp.servers[index].due = true;     // poll as soon as it is back,
            ntp.servers[index].resolve = true; // the addresses may have changed
        }
        ntp.polled_on_link = false;
        ntp.network_was_up = false;
        return;
    }
    if (!ntp.network_was_up)
    {
        ntp.network_was_up = true;
        ntp.network_up_ms = millis();
    }
    // The first poll on this link: once there is a global IPv6 address, or
    // it is clear there will not be one soon (an IPv4-only network).
    if (!ntp.polled_on_link && !net_ipv6_ready() && millis() - ntp.network_up_ms < IPV6_WAIT_MS)
    {
        return;
    }
    if (ntp.polling >= 0 || ntp_client_busy())
    {
        return;
    }
    index = next_due();
    if (index < 0)
    {
        return;
    }
    server = &ntp.servers[index];
    ntp.polled_on_link = true;
    server->due = false;
    server->resync = false;
    if (!resolve_if_needed(index))
    {
        NtpPollResult failed;

        memset(&failed, 0, sizeof(failed));
        failed.dns_failed = true;
        handle_poll(index, &failed);
        return;
    }
    int other = same_address_as(index, &server->targets[server->target]);
    if (other >= 0)
    {
        // Checked again at its next poll time, in case either has moved.
        set_aside(index, other);
        schedule_next(server, true);
        return;
    }
    if (server->same_as != 0)
    {
        ntp_event("server %d is no longer the same address as server %d\n", index,
                  server->same_as - 1);
        server->same_as = 0;
    }
    start_poll(index, &server->targets[server->target]);
}


bool ntp_take_report(SML_TIME_NTP_PAYLOAD *payload)
{
    if (!ntp.report_ready)
    {
        return false;
    }
    *payload = ntp.report;
    ntp.report_ready = false;
    return true;
}


// ---- Showing it ----------------------------------------------------------------------

static int configured_count(void)
{
    int count = 0;

    for (int index = 0; index < NTP_SLOTS; index++)
    {
        count += ntp.servers[index].configured ? 1 : 0;
    }
    return count;
}


static int verdict_count(NtpVerdict verdict)
{
    int count = 0;

    for (int index = 0; index < NTP_SLOTS; index++)
    {
        count += ntp.servers[index].verdict == verdict ? 1 : 0;
    }
    return count;
}


// How the servers stand together, after "; ": nothing with only one.
static void print_agreement(Stream *output)
{
    int configured = configured_count();

    if (configured < 2)
    {
        return;
    }
    output->printf("; %d servers", configured);
    if (ntp.disagree)
    {
        output->printf(", DISAGREE");
    }
    else if (verdict_count(NTP_VERDICT_FALSETICKER) != 0)
    {
        output->printf(", %d FALSETICKER", verdict_count(NTP_VERDICT_FALSETICKER));
    }
    if (verdict_count(NTP_VERDICT_UNUSABLE) != 0)
    {
        output->printf(", %d UNUSABLE", verdict_count(NTP_VERDICT_UNUSABLE));
    }
}


static void print_clock_line(Stream *output, const char *label)
{
    uint32_t utc;
    uint32_t bound_ns;

    output->printf("%s", label);
    if (!esp_clock_is_valid() || !ntp_utc_now(&utc))
    {
        output->printf("not set\n");
        return;
    }
    time_print_utc(output, utc);
    bound_ns = ntp_uncertainty_ns(now_us());
    if (bound_ns == UINT32_MAX)
    {
        output->printf(" +/- over 4.29 s");
    }
    else
    {
        output->printf(" +/-");
        time_print_ns(output, bound_ns);
    }
    output->printf(", %s\n", ntp_is_synced() ? "synced" : "NOT SYNCED");
}


// Two lines: the ESP's time and how well it is known, as the PIC's UTC
// line; then the server followed and how the others stand.
static void print_brief(Stream *output)
{
    const EspClockStatus *status = esp_clock_status();

    print_clock_line(output, "NTP:      ");
    output->printf("Server:   ");
    if (ntp.peer >= 0)
    {
        char name[SERVER_TEXT];

        output->printf("%s, stratum %u, updated ", server_text(&ntp.servers[ntp.peer], name),
                       status->last.stratum);
        time_print_duration(output, update_age_s());
        output->printf(" ago");
    }
    else
    {
        output->printf("NONE usable");
    }
    print_agreement(output);
    output->printf("\n");
}


static void print_us(Stream *output, int64_t us, bool plus)
{
    char text[TIME_NS_TEXT];

    time_format_ns(text, us * 1000, plus);
    output->printf(" %10s", text);
}


/*
 * Like chronyc sources: * the server followed, + one that agrees and is
 * combined with it, - one that agrees but is too far to combine, x one
 * the others outvote (or, without a majority, do not vouch for), ? one
 * without a recent good sample. Reach is the last eight polls in octal, as
 * chronyc shows it (377: all good). Offset is the latest sample's, against
 * the clock when it came; +/- its root distance now.
 */
void ntp_print_sources(Stream *output, bool verbose)
{
    int64_t now = now_us();

    output->printf("  # %-24s %2s %6s %5s %6s %10s %10s", "Server", "St", "Poll", "Reach", "LastRx",
                   "Offset", "+/-");
    if (verbose)
    {
        output->printf(" %10s %10s", "Delay", "Jitter");
    }
    output->printf("  Address\n");
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        const NtpServer *server = &ntp.servers[index];
        char age[TIME_AGE_TEXT];

        if (!server->configured)
        {
            if (verbose)
            {
                output->printf("  %d -\n", index);
            }
            continue;
        }
        output->printf("%c %d %-24.24s",
                       server->same_as != 0
                           ? '='
                           : ntp_verdict_marker(server->verdict, ntp.selection.in_combined[index]),
                       index, server->host);
        if (server->count == 0)
        {
            output->printf(" %2s %5lus %5o %6s %10s %10s", "-", (unsigned long)ntp.interval_s,
                           server->reach, "-", "-", "-");
            if (verbose)
            {
                output->printf(" %10s %10s", "-", "-");
            }
        }
        else
        {
            const EspClockSample *sample = latest(server);

            time_format_age(age, (uint32_t)((now - sample->local_us) / 1000000));
            output->printf(" %2u %5lus %5o %6s", sample->stratum, (unsigned long)ntp.interval_s,
                           server->reach, age);
            print_us(output, server->measured_us, true);
            print_us(output, server->distance_us, false);
            if (verbose)
            {
                print_us(output, sample->delay_us, false);
                print_us(output, server->jitter_us, false);
            }
        }
        output->printf("  %s", server->address[0] != '\0' ? server->address : "-");
        if (server->same_as != 0)
        {
            int other = server->same_as - 1;

            output->printf(", as server %d", other);
            if (user_prefs_ntp_offset_us[index] != user_prefs_ntp_offset_us[other])
            {
                output->printf(" (its offset %+ld us used, not %+ld)",
                               (long)user_prefs_ntp_offset_us[other],
                               (long)user_prefs_ntp_offset_us[index]);
            }
        }
        output->printf("\n");
    }
    if (ntp.disagree)
    {
        output->printf("Servers DISAGREE: no majority, following the one in use\n");
    }
    if (!verbose)
    {
        return;
    }
    ntp_print_stats(output);
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        const NtpServer *server = &ntp.servers[index];

        if (!server->configured)
        {
            continue;
        }
        output->printf("  %d: %lu polls (%lu good), %lu to the other address family, "
                       "resolved %lu times, %lu DNS failures, statistics restarted %lu times",
                       index, (unsigned long)server->polls, (unsigned long)server->good_polls,
                       (unsigned long)server->fallbacks, (unsigned long)server->resolves,
                       (unsigned long)server->dns_failures, (unsigned long)server->restarts);
        if (server->kisses_rate + server->kisses_refused + server->kisses_other != 0)
        {
            output->printf("; kisses: %lu RATE, %lu DENY or RSTR, %lu other",
                           (unsigned long)server->kisses_rate, (unsigned long)server->kisses_refused,
                           (unsigned long)server->kisses_other);
        }
        if (server->rate_shift != 0)
        {
            output->printf("; polled every %lu s (RATE)", (unsigned long)server_interval_s(server));
        }
        if (server->refused)
        {
            output->printf("; REFUSED (%s), not polled", server->refused_code);
        }
        if (server->have_last && !server->last.ok)
        {
            output->printf("; last poll failed (%s%s%s)", poll_failure(&server->last),
                           server->last.kiss[0] != '\0' ? " " : "", server->last.kiss);
        }
        output->printf("\n");
    }
}


/*
 * Like chronyc sourcestats: each server's line. Kept: samples in the line,
 * Runs: the runs among their residuals, Span: from the oldest in the line
 * to the latest, Freq: the line's slope as the ESP's frequency error
 * (+ = fast), Skew: its bound, Offset: the line now against the clock,
 * SD: the samples' spread about the line.
 */
void ntp_print_stats(Stream *output)
{
    int64_t now = now_us();

    output->printf("  # %-24s %4s %4s %6s %10s %10s %10s %10s\n", "Server", "Kept", "Runs", "Span",
                   "Freq ppm", "Skew ppm", "Offset", "SD");
    for (int index = 0; index < NTP_SLOTS; index++)
    {
        const NtpServer *server = &ntp.servers[index];
        const NtpStats *stats = &server_stats[index];
        char span[TIME_AGE_TEXT];

        if (!server->configured || stats->samples == 0)
        {
            continue;
        }
        time_format_age(span, (uint32_t)(ntp_stats_span_us(stats) / 1000000));
        output->printf("  %d %-24.24s %4u %4u %6s", index, server->host, stats->samples,
                       stats->runs, span);
        if (stats->fitted)
        {
            output->printf(" %+10.3f %10.3f", -stats->frequency_ppb / 1000.0,
                           stats->skew_ppb / 1000.0);
        }
        else
        {
            output->printf(" %10s %10s", "-", "-");
        }
        print_us(output, esp_clock_is_valid() ? line_against_clock(index, now) : 0, true);
        print_us(output, stats->fitted ? (int64_t)llround(stats->sd_us) : 0, false);
        output->printf("\n");
    }
}


// Like chronyc tracking: the server followed and the clock's state.
void ntp_print_tracking(Stream *output)
{
    const EspClockStatus *status = esp_clock_status();

    output->printf("Server:    ");
    if (ntp.peer >= 0)
    {
        char name[SERVER_TEXT];

        output->printf("%d %s, stratum %u", ntp.peer, server_text(&ntp.servers[ntp.peer], name),
                       status->last.stratum);
    }
    else
    {
        output->printf("NONE usable");
    }
    print_agreement(output);
    output->printf("\n");
    if (ntp.selection.combined > 1)
    {
        output->printf("Combined:  %d servers, offset ", ntp.selection.combined);
        time_print_ns(output, ntp.selection.combined_offset_us * 1000);
        output->printf(" (the followed one's ");
        time_print_ns(output, ntp.servers[ntp.peer].offset_us * 1000);
        output->printf("); %lu updates combined\n", (unsigned long)ntp.combined_updates);
    }
    print_clock_line(output, "Clock:     ");
    if (!esp_clock_is_valid())
    {
        return;
    }
    output->printf("Updated:   ");
    time_print_utc(output, (uint32_t)(status->last_update_utc_us / 1000000));
    output->printf(", ");
    time_print_duration(output, update_age_s());
    output->printf(" ago\nLast:      sample ");
    time_print_ns(output, status->last_error_us * 1000);
    output->printf(", correction ");
    time_print_ns(output, status->last_correction_us * 1000);
    output->printf(", round trip ");
    time_print_ns(output, status->last.delay_us * 1000);
    output->printf("\nRoot:      delay ");
    time_print_ns(output, (int64_t)status->last.root_delay_us * 1000);
    output->printf(", dispersion ");
    time_print_ns(output, (int64_t)status->last.root_dispersion_us * 1000);
    output->printf("\nFrequency: %+.3f ppm", -(float)status->frequency_ppb / 1000.0f);
    if (status->skew_ppb >= 0)
    {
        output->printf(", skew %.3f (%lu estimates, the servers' lines)",
                       (float)status->skew_ppb / 1000.0f, (unsigned long)status->frequency_samples);
    }
    else
    {
        if (status->frequency_samples != 0 && status->frequency_error_ppb >= 0)
        {
            output->printf(" +/- %.3f", (float)status->frequency_error_ppb / 1000.0f);
        }
        output->printf(" (%lu estimates, latest from %u samples)",
                       (unsigned long)status->frequency_samples, status->frequency_used);
    }
    output->printf("; drift allowed %.2f ppm\n", (float)esp_clock_drift_ppb() / 1000.0f);
    output->printf("Slewing:   ");
    time_print_ns(output, status->slew_us * 1000);
    if (now_us() < status->slew_end_us)
    {
        output->printf(" %s, done in ", status->fast ? "fast" : "gently");
        time_print_duration(output, (uint32_t)((status->slew_end_us - now_us()) / 1000000));
    }
    else
    {
        output->printf(", done");
    }
    output->printf(" (%lu fast)\n", (unsigned long)status->fast_slews);
    TempCompPolls polls = temp_comp_short_polls(now_us());

    output->printf("Interval:  %lu s (adaptive, at most %lu s", (unsigned long)ntp.interval_s,
                   (unsigned long)ntp.max_interval_s);
    if (polls != TEMP_COMP_POLLS_NORMAL)
    {
        output->printf(", short: %s, %lu s", temp_comp_polls_name(polls),
                       (unsigned long)short_polls_s(polls));
    }
    output->printf(")\n");
    // The corrections set by hand: ntp asymmetry, then each ntp server offset.
    output->printf("Asymmetry: %+ld us", (long)user_prefs_ntp_asymmetry_us);
    for (int server = 0; server < NTP_SLOTS; server++)
    {
        if (user_prefs_ntp_offset_us[server] != 0)
        {
            output->printf(", server %d %+ld us", server, (long)user_prefs_ntp_offset_us[server]);
        }
    }
    output->printf(" (+ = clock later)\n");
}


void ntp_print(Stream *output, bool verbose)
{
    const NtpClientStats *stats = ntp_client_stats();
    const EspClockStatus *status = esp_clock_status();
    const PpsOutputStats *pps = pps_output_stats();

    if (!verbose)
    {
        print_brief(output);
        return;
    }
    ntp_print_tracking(output);
    ntp_print_sources(output, true);
    output->printf("Polls:     %lu (%lu good), %lu exchanges, %lu timeouts, %lu updates, "
                   "%lu steps\n",
                   (unsigned long)stats->polls, (unsigned long)stats->good_polls,
                   (unsigned long)stats->exchanges, (unsigned long)stats->timeouts,
                   (unsigned long)status->updates, (unsigned long)status->steps);
    output->printf("ESP PPS:   %s, %lu edges, %lu planned, %lu ran on, latency %ld us (max %ld), "
                   "armed %ld us early\n",
                   pps_output_running() ? "running" : "stopped", (unsigned long)ntp.edges,
                   (unsigned long)pps->planned, (unsigned long)pps->extrapolated,
                   (long)pps->latency_us, (long)pps->latency_max_us, (long)pps->lead_us);
    if (ntp.edges != 0)
    {
        output->printf("Last edge: UTC + ");
        time_print_ns(output, (int64_t)ntp.report.utc_phase_ns);
        output->printf("\n");
    }
}
