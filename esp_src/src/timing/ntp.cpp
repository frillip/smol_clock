#include "ntp.h"

#include <Network.h>
#include <esp_timer.h>
#include <math.h>
#include <strings.h>
#include <sys/time.h>

#include "../console/console.h"
#include "../net/net_connect.h"
#include "../settings/user_prefs.h"
#include "../util/time_text.h"
#include "esp_clock.h"
#include "ntp_client.h"
#include "ntp_select.h"
#include "pps.h"

#define LEARN_FIRST_S     64U
#define POLL_MIN_S        128U // adaptive range, once learnt: from here ...
#define POLL_START_S      256U
#define POLL_QUIET_US     500LL // ... longer after POLL_QUIET_RUN corrections under this
#define POLL_QUIET_RUN    4U
#define POLL_LOUD_US      2000LL // ... shorter after one over this
#define RETRY_FIRST_S     60U
#define SYNC_SLACK_S      300U
#define FREE_RUN_AFTER_S  86400U
#define PPS_SYNC_LIMIT_NS 1000000 // phase within which the edge marks its second
// The first poll on a link waits this long at most for SLAAC's global IPv6
// address, which follows IPv4 by a few seconds: started on IPv4, the
// frequency would be learnt on one path and continued on another.
#define IPV6_WAIT_MS 15000U
#define FED_BY_OTHER (-2)
// A server's samples kept, for its jitter.
#define HISTORY 8
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
    // This poll's addresses, preferred family first, and the one in use: a
    // poll with no reply at all goes on to the next at once.
    IPAddress targets[NET_ADDRESSES];
    int target_count;
    int target;
    char address[48]; // the address last polled
    EspClockSample history[HISTORY];
    uint8_t count;
    uint8_t next;
    int64_t measured_us;      // the latest sample against the clock when it came
    int64_t applied_local_us; // the latest sample given to the clock, if any
    NtpPollResult last;       // the latest poll, good or bad
    bool have_last;
    // From the latest selection.
    int64_t offset_us;
    int64_t distance_us;
    int64_t jitter_us;
    NtpVerdict verdict;
    uint32_t polls;
    uint32_t good_polls;
    uint32_t fallbacks;
    uint32_t dns_failures;
} NtpServer;

typedef struct
{
    NtpServer servers[NTP_SERVERS];
    int peer;    // the server the clock follows, -1 if none
    int polling; // the server with a poll in flight, -1 if none
    int fed_by;  // the server whose samples the frequency window holds: -1 none
                 // yet, FED_BY_OTHER one since changed
    NtpSelection selection;
    uint32_t combined_updates; // clock updates from more than one server
    bool disagree;           // reported: the usable servers have no majority
    uint32_t max_interval_s; // ntp.interval: the longest poll interval
    uint32_t interval_s;     // the interval in use, adaptive below that
    uint32_t quiet;          // consecutive small corrections
    bool report_ready;
    SML_TIME_NTP_PAYLOAD report;
    uint32_t edges;
    int64_t last_edge_utc_us;
    bool polled_on_link; // a poll has been started since the network came up
    bool network_was_up;
    uint32_t network_up_ms; // when it came up
} Ntp;

static Ntp ntp;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}


static void reset_server(int index)
{
    NtpServer *server = &ntp.servers[index];

    *server = NtpServer(); // zeroed, with IPAddress constructed
    snprintf(server->host, sizeof(server->host), "%s", user_prefs_ntp_server(index));
    server->configured = server->host[0] != '\0';
    server->due = true;
    server->retry_s = RETRY_FIRST_S;
    server->verdict = server->configured ? NTP_VERDICT_UNUSABLE : NTP_VERDICT_NONE;
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
    ntp_client_init();
    ntp.peer = -1;
    ntp.polling = -1;
    ntp.fed_by = -1;
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        reset_server(index);
    }
    ntp.max_interval_s = user_prefs.fields.ntp.interval;
    ntp.interval_s = POLL_START_S < ntp.max_interval_s ? POLL_START_S : ntp.max_interval_s;
}


// A poll of every server now: or, for one being polled, as soon as it ends,
// which would otherwise schedule the next for an interval on.
void ntp_resync(void)
{
    for (int index = 0; index < NTP_SERVERS; index++)
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


/*
 * The poll interval adapts between POLL_MIN_S and ntp.interval: it doubles
 * after POLL_QUIET_RUN corrections in a row under POLL_QUIET_US, and halves
 * after one over POLL_LOUD_US. Short polls while the crystal wanders (warm
 * up, temperature) keep each correction small enough to slew gently; long
 * ones once it is steady spare the servers. Only the corrections from the
 * server followed count; every server is polled at the interval.
 */
static void adapt_interval(int64_t error_us)
{
    uint32_t longest = ntp.max_interval_s;
    uint32_t shortest = POLL_MIN_S < longest ? POLL_MIN_S : longest;

    if (error_us < 0)
    {
        error_us = -error_us;
    }
    if (error_us > POLL_LOUD_US)
    {
        ntp.interval_s /= 2;
        ntp.quiet = 0;
    }
    else if (error_us < POLL_QUIET_US && ++ntp.quiet >= POLL_QUIET_RUN)
    {
        ntp.interval_s *= 2;
        ntp.quiet = 0;
    }
    if (ntp.interval_s > longest)
    {
        ntp.interval_s = longest;
    }
    if (ntp.interval_s < shortest)
    {
        ntp.interval_s = shortest;
    }
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
    if (esp_clock_status()->frequency_samples < 2 && server->learn_step < 3)
    {
        delay_s = LEARN_FIRST_S << server->learn_step++;
        return delay_s < ntp.interval_s ? delay_s : ntp.interval_s;
    }
    return ntp.interval_s;
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

static const EspClockSample *latest(const NtpServer *server)
{
    return &server->history[(server->next + HISTORY - 1) % HISTORY];
}


// A sample's offset against the clock as it now stands: the server's view
// of our error at the time it was taken.
static int64_t against_clock(const EspClockSample *sample)
{
    return sample->local_us + sample->offset_us - esp_clock_utc_us(sample->local_us);
}


/*
 * The spread of a server's recent samples: the standard deviation of their
 * raw offsets about a straight line through them, so that the ESP crystal's
 * frequency error (the line's slope) is not counted against the server.
 * 0 with fewer than three.
 */
static int64_t jitter_us(const NtpServer *server)
{
    double sum_t = 0.0, sum_x = 0.0, sum_tt = 0.0, sum_tx = 0.0;
    double t0 = (double)server->history[0].local_us;
    double x0 = (double)server->history[0].offset_us;
    double n = server->count;
    double slope;
    double intercept;
    double squares = 0.0;
    uint8_t index;

    if (server->count < 3)
    {
        return 0;
    }
    for (index = 0; index < server->count; index++)
    {
        double t = (double)server->history[index].local_us - t0;
        double x = (double)server->history[index].offset_us - x0;

        sum_t += t;
        sum_x += x;
        sum_tt += t * t;
        sum_tx += t * x;
    }
    if (n * sum_tt - sum_t * sum_t <= 0.0)
    {
        return 0;
    }
    slope = (n * sum_tx - sum_t * sum_x) / (n * sum_tt - sum_t * sum_t);
    intercept = (sum_x - slope * sum_t) / n;
    for (index = 0; index < server->count; index++)
    {
        double t = (double)server->history[index].local_us - t0;
        double residual = (double)server->history[index].offset_us - x0 - (intercept + slope * t);

        squares += residual * residual;
    }
    return (int64_t)sqrt(squares / (n - 2.0));
}


/*
 * Root distance, as NTP has it: how far the server's time can be from the
 * truth, seen from here. Half the round trip and half the server's own root
 * delay (the path's asymmetry could be all of it), its root dispersion, the
 * jitter of its samples, and the drift allowed since the latest.
 */
static int64_t root_distance_us(const NtpServer *server, int64_t now)
{
    const EspClockSample *sample = latest(server);
    int64_t age = now - sample->local_us;
    int64_t distance;

    if (age < 0)
    {
        age = 0;
    }
    distance = sample->delay_us / 2 + (int64_t)sample->root_delay_us / 2 +
               (int64_t)sample->root_dispersion_us + server->jitter_us +
               age * esp_clock_drift_ppm() / 1000000;
    return distance > 0 ? distance : 1;
}


static bool usable(const NtpServer *server, int64_t now)
{
    int64_t limit_us = (2LL * ntp.max_interval_s + SYNC_SLACK_S) * 1000000;

    return server->configured && server->count != 0 && (server->reach & REACH_RECENT) != 0 &&
           now - latest(server)->local_us <= limit_us;
}


// The server's latest sample to the clock, unless it has had it. Its
// offset is the servers' combined one when there is one (ntp_combine()):
// the followed server's sample moved by the difference.
static void apply(int index)
{
    NtpServer *server = &ntp.servers[index];
    EspClockSample combined = *latest(server);
    const EspClockSample *sample = &combined;
    const EspClockStatus *status = esp_clock_status();
    EspClockAction action;

    if (server->count == 0 || server->applied_local_us == sample->local_us)
    {
        return;
    }
    if (esp_clock_is_valid() && ntp.selection.selected == index && ntp.selection.combined > 1)
    {
        combined.offset_us += ntp.selection.combined_offset_us - server->offset_us;
        ntp.combined_updates++;
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
    action = esp_clock_update(sample, now_us(), (int64_t)ntp.interval_s * 2000000);
    set_system_time();
    if (action == ESP_CLOCK_STEPPED)
    {
        char step[TIME_NS_TEXT];
        char name[SERVER_TEXT];

        time_format_ns(step, status->last_error_us * 1000, true);
        console_printf("NTP: clock set from %s, step %s (%u steps)\n", server_text(server, name),
                       step, (unsigned)status->steps);
        start_pps();
    }
    else if (action == ESP_CLOCK_SLEWED_FAST)
    {
        char slew[TIME_NS_TEXT];

        time_format_ns(slew, status->slew_us * 1000, true);
        console_printf("NTP: correcting %s at %d ppm\n", slew, ESP_CLOCK_FAST_SLEW_PPM);
    }
    if (status->frequency_samples >= 2 && action != ESP_CLOCK_STEPPED)
    {
        adapt_interval(status->last_error_us);
    }
}


/*
 * Chooses the server to follow (ntp_select.h) after any poll, and gives the
 * clock that server's latest sample if it has not had it: fresh is the
 * server just polled. The first good sample of all sets the clock, there
 * being nothing to judge it against.
 */
static void select_server(int fresh)
{
    NtpCandidate candidates[NTP_SERVERS];
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
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        NtpServer *server = &ntp.servers[index];

        candidates[index].configured = server->configured;
        candidates[index].usable = usable(server, now);
        if (server->count != 0)
        {
            server->jitter_us = jitter_us(server);
            server->offset_us = against_clock(latest(server));
            server->distance_us = root_distance_us(server, now);
        }
        candidates[index].offset_us = server->offset_us;
        candidates[index].distance_us = server->distance_us;
    }
    ntp_select(candidates, NTP_SERVERS, ntp.peer, &ntp.selection);
    ntp_combine(candidates, NTP_SERVERS, &ntp.selection);
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        ntp.servers[index].verdict = ntp.selection.verdict[index];
    }

    ntp.peer = ntp.selection.selected;
    if (ntp.peer >= 0 && ntp.peer != previous)
    {
        char name[SERVER_TEXT];

        console_printf("NTP: following %s\n", server_text(&ntp.servers[ntp.peer], name));
    }
    else if (ntp.peer < 0 && previous >= 0)
    {
        console_printf("NTP: no server usable, coasting\n");
    }
    if (ntp.peer >= 0)
    {
        apply(ntp.peer);
    }

    disagree = !ntp.selection.majority && ntp.selection.usable >= 2;
    if (disagree != ntp.disagree)
    {
        ntp.disagree = disagree;
        console_printf(disagree ? "NTP: servers DISAGREE, no majority\n" : "NTP: servers agree\n");
    }
}


static void handle_poll(int index, const NtpPollResult *poll)
{
    NtpServer *server = &ntp.servers[index];

    server->last = *poll;
    server->have_last = true;
    server->polls++;
    server->reach = (uint8_t)(server->reach << 1 | (poll->ok ? 1U : 0U));
    if (!poll->ok)
    {
        console_printf("NTP: poll of %s failed (%s %s)\n", server->host, poll_failure(poll),
                       poll->kiss);
        schedule_next(server, false);
        select_server(-1);
        return;
    }
    server->good_polls++;
    server->measured_us = esp_clock_is_valid() ? against_clock(&poll->sample) : 0;
    server->history[server->next] = poll->sample;
    server->next = (uint8_t)((server->next + 1) % HISTORY);
    if (server->count < HISTORY)
    {
        server->count++;
    }
    select_server(index);
    schedule_next(server, true);
}


// ---- TIME-NTP and the ESP PPS ---------------------------------------------------

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
        report->lock_state = status->frequency_samples != 0 ? SML_LOCK_LOCKED : SML_LOCK_ACQUIRING;
    }
    else
    {
        report->lock_state = age_s > FREE_RUN_AFTER_S ? SML_LOCK_FREE_RUNNING : SML_LOCK_HOLDOVER;
    }
    report->utc_seconds = (uint32_t)second;
    report->utc_phase_ns = (int32_t)phase_ns;
    report->uncertainty_ns = esp_clock_uncertainty_ns(edge->local_us);
    report->pps_sequence = edge->sequence;
    report->last_update_utc = (uint32_t)(esp_clock_utc_us(status->last_update_local_us) / 1000000);
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


// A server's setting changed (ntp server, defaults): it starts again.
static void check_settings(void)
{
    if (ntp.max_interval_s != user_prefs.fields.ntp.interval)
    {
        ntp.max_interval_s = user_prefs.fields.ntp.interval;
        if (ntp.interval_s > ntp.max_interval_s)
        {
            ntp.interval_s = ntp.max_interval_s;
        }
    }
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        if (index != ntp.polling &&
            strcmp(ntp.servers[index].host, user_prefs_ntp_server(index)) != 0)
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
    if (strcmp(server->host, user_prefs_ntp_server(ntp.polling)) != 0)
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
        console_printf("NTP: no reply from %s, trying %s\n", poll.address,
                       server->targets[server->target].toString().c_str());
        start_poll(index, &server->targets[server->target]);
        return;
    }
    handle_poll(index, &poll);
}


// The server to poll next: due, the longest overdue first.
static int next_due(void)
{
    int64_t now = now_us();
    int chosen = -1;

    for (int index = 0; index < NTP_SERVERS; index++)
    {
        const NtpServer *server = &ntp.servers[index];

        if (!server->configured || (!server->due && now < server->next_poll_us))
        {
            continue;
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


void ntp_service(bool network_up)
{
    NtpServer *server;
    int index;

    check_settings();
    take_result(network_up);
    service_pps();

    if (!network_up)
    {
        for (index = 0; index < NTP_SERVERS; index++)
        {
            ntp.servers[index].due = true; // poll as soon as it is back
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
    // Resolved here, in the loop task (ntp_client.h): both families, the
    // preferred first (net_connect.h).
    server->target_count = net_resolve(server->host, server->targets);
    server->target = 0;
    if (server->target_count == 0)
    {
        NtpPollResult failed;

        memset(&failed, 0, sizeof(failed));
        failed.dns_failed = true;
        server->dns_failures++;
        handle_poll(index, &failed);
        return;
    }
    start_poll(index, &server->targets[0]);
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

    for (int index = 0; index < NTP_SERVERS; index++)
    {
        count += ntp.servers[index].configured ? 1 : 0;
    }
    return count;
}


static int verdict_count(NtpVerdict verdict)
{
    int count = 0;

    for (int index = 0; index < NTP_SERVERS; index++)
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

    output->printf("%s", label);
    if (!esp_clock_is_valid() || !ntp_utc_now(&utc))
    {
        output->printf("not set\n");
        return;
    }
    time_print_utc(output, utc);
    output->printf(" +/-");
    time_print_ns(output, esp_clock_uncertainty_ns(now_us()));
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
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        const NtpServer *server = &ntp.servers[index];
        char age[TIME_AGE_TEXT];

        if (!server->configured)
        {
            if (verbose)
            {
                output->printf("  %d -\n", index + 1);
            }
            continue;
        }
        output->printf("%c %d %-24.24s",
                       ntp_verdict_marker(server->verdict, ntp.selection.in_combined[index]),
                       index + 1,
                       server->host);
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
        output->printf("  %s\n", server->address[0] != '\0' ? server->address : "-");
    }
    if (ntp.disagree)
    {
        output->printf("Servers DISAGREE: no majority, following the one in use\n");
    }
    if (!verbose)
    {
        return;
    }
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        const NtpServer *server = &ntp.servers[index];

        if (!server->configured)
        {
            continue;
        }
        output->printf("  %d: %lu polls (%lu good), %lu to the other address family, "
                       "%lu DNS failures",
                       index + 1, (unsigned long)server->polls, (unsigned long)server->good_polls,
                       (unsigned long)server->fallbacks, (unsigned long)server->dns_failures);
        if (server->have_last && !server->last.ok)
        {
            output->printf("; last poll failed (%s %s)", poll_failure(&server->last),
                           server->last.kiss);
        }
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

        output->printf("%d %s, stratum %u", ntp.peer + 1, server_text(&ntp.servers[ntp.peer], name),
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
    time_print_utc(output, (uint32_t)(esp_clock_utc_us(status->last_update_local_us) / 1000000));
    output->printf(", ");
    time_print_duration(output, update_age_s());
    output->printf(" ago\nLast:      correction ");
    time_print_ns(output, status->last_error_us * 1000);
    output->printf(", round trip ");
    time_print_ns(output, status->last.delay_us * 1000);
    output->printf("\nRoot:      delay ");
    time_print_ns(output, (int64_t)status->last.root_delay_us * 1000);
    output->printf(", dispersion ");
    time_print_ns(output, (int64_t)status->last.root_dispersion_us * 1000);
    output->printf("\nFrequency: %+.3f ppm (%lu estimates, latest from %u samples)\n",
                   -(float)status->frequency_ppb / 1000.0f,
                   (unsigned long)status->frequency_samples, status->frequency_used);
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
    output->printf("Interval:  %lu s (adaptive, at most %lu s)\n", (unsigned long)ntp.interval_s,
                   (unsigned long)ntp.max_interval_s);
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
    output->printf("ESP PPS:   %s, %lu edges, %lu planned, %lu ran on, latency %ld us (max %ld)\n",
                   pps_output_running() ? "running" : "stopped", (unsigned long)ntp.edges,
                   (unsigned long)pps->planned, (unsigned long)pps->extrapolated,
                   (long)pps->latency_us, (long)pps->latency_max_us);
    if (ntp.edges != 0)
    {
        output->printf("Last edge: UTC + ");
        time_print_ns(output, (int64_t)ntp.report.utc_phase_ns);
        output->printf("\n");
    }
}
