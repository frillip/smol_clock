/*
 * ntp.cpp with up to four servers, against fake servers and a fake client
 * that answers at once: each server's offset is the truth plus its own
 * bias. Which server is followed, the verdicts, the clock's error, and the
 * tables.
 */
#include <stdio.h>
#include <string.h>
#include <string>

#include "../../src/console/console.h"
#include "../../src/net/dhcp_ntp.h"
#include "../../src/net/net_connect.h"
#include "../../src/settings/user_prefs.h"
#include "../../src/timing/esp_clock.h"
#include "../../src/timing/ntp.h"
#include "../../src/timing/ntp_client.h"
#include "../../src/timing/ntp_packet.h"
#include "../../src/timing/temp_comp.h"
#include "../../src/timing/pps.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

uint32_t fake_millis;
int64_t fake_now_us = 1000000;
static const int64_t TRUTH_US = 1800000000LL * 1000000; // UTC - local

int fake_settimeofday(const struct timeval *, const void *)
{
    return 0;
}


static std::string console;
void console_printf(const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    console += text;
}


// ---- Settings ----------------------------------------------------------------------

UserPrefs user_prefs;
UserPrefsNtp user_prefs_ntp;
UserPrefsTcomp user_prefs_esp_tcomp = {1, TEMP_COMP_NOT_SET, 0, 0};
void user_prefs_save_tcomp_learnt(void) {}
int32_t user_prefs_ntp_asymmetry_us;
int32_t user_prefs_ntp_dhcp = NTP_DHCP_DISABLED; // on for its own section
int32_t user_prefs_ntp_offset_us[NTP_SERVERS + 1];

// DHCP's NTP server: none offered until the test offers one.
static bool dhcp_offered;
static IPAddress dhcp_address;
bool dhcp_ntp_server(IPAddress *address)
{
    if (dhcp_offered)
    {
        *address = dhcp_address;
    }
    return dhcp_offered;
}
char *user_prefs_ntp_server(int index)
{
    return index == 0 ? user_prefs.fields.ntp.server : user_prefs_ntp.server[index - 1];
}


void user_prefs_set_string(char *field, const char *value)
{
    memset(field, 0, USER_PREFS_STORED_STRING_SIZE);
    strncpy(field, value, USER_PREFS_MAX_STRING_SIZE);
}


// ---- Servers: s<n>.example is 192.0.2.n ---------------------------------------------

typedef struct
{
    bool answers;
    int64_t bias_us;
    int64_t delay_us;
    uint32_t polls;
    const char *kiss; // answers with this kiss-o'-death instead, if set
    // The host behind the address (0: replies carry no identity), and how
    // often it updates its own clock, which its reference timestamp says.
    int host;
    uint32_t reference_period_s;
} FakeServer;

static FakeServer fake[10];

// pool.example: another address at each lookup, 192.0.2.7 then .8, as a
// pool or anycast name does.
static unsigned pool_lookups;

int net_resolve(const char *host, IPAddress addresses[NET_ADDRESSES])
{
    unsigned n;

    if (strcmp(host, "pool.example") == 0)
    {
        addresses[0] = IPAddress(IPv4, (uint8_t)(7 + pool_lookups++ % 2));
        return 1;
    }
    if ((sscanf(host, "s%u.example", &n) != 1 && sscanf(host, "192.0.2.%u", &n) != 1) || n >= 10)
    {
        return 0;
    }
    addresses[0] = IPAddress(IPv4, (uint8_t)n);
    return 1;
}


bool net_ipv6_ready(void)
{
    return true;
}


void net_note_result(const IPAddress &, bool)
{
}


// ---- The client: answers at once -------------------------------------------------------

static bool ready;
static NtpPollResult result;
static NtpClientStats stats;

void ntp_client_init(void)
{
}


bool ntp_client_poll(const NtpServerAddress *address)
{
    FakeServer *server = &fake[address->bytes[3]];

    memset(&result, 0, sizeof result);
    snprintf(result.address, sizeof result.address, "192.0.2.%u", address->bytes[3]);
    server->polls++;
    stats.polls++;
    if (server->answers && server->kiss != NULL)
    {
        result.last_result = NTP_REPLY_KISS;
        snprintf(result.kiss, sizeof result.kiss, "%s", server->kiss);
    }
    else if (server->answers)
    {
        result.ok = true;
        result.replies = 4;
        result.sample.local_us = fake_now_us;
        result.sample.offset_us = TRUTH_US + server->bias_us;
        result.sample.delay_us = server->delay_us;
        result.sample.root_delay_us = 1000;
        result.sample.root_dispersion_us = 500;
        result.sample.stratum = 2;
        if (server->host != 0)
        {
            // Every host stratum 2 from the same upstream, the same root
            // delay: only the reference timestamp tells them apart.
            uint64_t period = server->reference_period_s != 0 ? server->reference_period_s : 1024;
            uint64_t phase = (uint64_t)server->host * 37;
            uint64_t utc_s = (uint64_t)((fake_now_us + TRUTH_US) / 1000000);
            uint64_t seconds = (utc_s - phase) / period * period + phase + 2208988800ULL;

            result.first.valid = true;
            result.first.stratum = 2;
            result.first.refid = 0xC0000201;
            result.first.root_delay = 0x42;
            result.first.reference = seconds << 32 | (0x10000000ULL * (uint64_t)server->host + 1);
            result.last = result.first;
        }
    }
    ready = true;
    return true;
}


bool ntp_client_busy(void)
{
    return false;
}


bool ntp_client_take(NtpPollResult *destination)
{
    if (!ready)
        return false;
    *destination = result;
    ready = false;
    return true;
}


const NtpClientStats *ntp_client_stats(void)
{
    return &stats;
}


// ---- The PPS: not run ----------------------------------------------------------------

static PpsOutputStats pps_stats;
void pps_output_start(int64_t, int32_t)
{
}


void pps_output_plan(int64_t, int32_t)
{
}


bool pps_output_running(void)
{
    return false;
}


// Off, or an edge each second of fake time: then TIME-NTP reports are built.
static bool pps_on;
static int64_t pps_last_us;
static uint32_t pps_sequence;
bool pps_output_take(PpsEdge *edge)
{
    if (!pps_on || fake_now_us == pps_last_us)
    {
        return false;
    }
    pps_last_us = fake_now_us;
    edge->local_us = fake_now_us;
    edge->sequence = ++pps_sequence;
    return true;
}


const PpsOutputStats *pps_output_stats(void)
{
    return &pps_stats;
}


// ---- Running it ------------------------------------------------------------------------

static void run_s(int seconds)
{
    for (int i = 0; i < seconds; i++)
    {
        fake_now_us += 1000000;
        fake_millis += 1000;
        ntp_service(true);
        ntp_service(true); // the result, taken
    }
}


static uint32_t all_polls(void)
{
    uint32_t polls = 0;

    for (int n = 0; n < 10; n++)
    {
        polls += fake[n].polls;
    }
    return polls;
}


static bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}


static std::string sources(void)
{
    Stream out;
    ntp_print_sources(&out, false);
    return out.out;
}


// The clock's error now, us.
static int64_t error_us(void)
{
    return esp_clock_utc_us(fake_now_us) - (fake_now_us + TRUTH_US);
}


static int64_t magnitude(int64_t value)
{
    return value < 0 ? -value : value;
}


int main(void)
{
    std::string table;

    user_prefs_set_string(user_prefs.fields.ntp.server, "s1.example");
    user_prefs.fields.ntp.interval = 1024;
    for (int n = 0; n < 10; n++)
    {
        fake[n].answers = true;
        fake[n].delay_us = 10000;
    }
    fake[1].bias_us = 300;

    printf("One server sets the clock\n");
    ntp_init();
    run_s(3);
    CHECK(fake[1].polls == 1 && esp_clock_is_valid() && magnitude(error_us() - 300) < 10);
    CHECK(contains(console, "NTP: clock set from s1.example (192.0.2.1)"));
    table = sources();
    CHECK(contains(table, "* 1 s1.example") && contains(table, "192.0.2.1"));
    CHECK(!contains(table, "s2.example")); // servers not set are left out
    run_s(64);
    CHECK(fake[1].polls == 2);

    printf("Three that agree\n");
    fake[2].bias_us = -200;
    fake[3].bias_us = 500;
    user_prefs_set_string(user_prefs_ntp_server(1), "s2.example");
    user_prefs_set_string(user_prefs_ntp_server(2), "s3.example");
    run_s(5);
    CHECK(fake[2].polls == 1 && fake[3].polls == 1);
    table = sources();
    // Not selectable until each has a line (three samples): s1, which set
    // the clock, is kept meanwhile.
    CHECK(contains(table, "* 1 s1.example") && contains(table, "? 2 s2.example") &&
          contains(table, "? 3 s3.example"));
    CHECK(!contains(console, "NTP: following s2") && !contains(console, "NTP: following s3"));
    CHECK(!ntp_servers_disagree());
    // The first, and the first to agree, is followed; the three are
    // combined, equally weighted: (300 - 200 + 500) / 3.
    run_s(5000);
    printf("  clock error %+lld us, combined from 3 at +300, -200, +500\n", (long long)error_us());
    CHECK(contains(sources(), "* 1 s1.example") && magnitude(error_us() - 200) < 30);
    {
        Stream out;
        ntp_print_tracking(&out);
        CHECK(contains(out.out, "Combined:  3 servers"));
        // Steered by the servers' lines, the polls lengthening as the
        // lines grow (chronyd's poll rule), from the shortest after learning.
        CHECK(contains(out.out, "the servers' lines") && esp_clock_status()->skew_ppb >= 0);
        CHECK(contains(out.out, "Interval:  256 s") || contains(out.out, "Interval:  512 s"));
        out.out.clear();
        ntp_print_stats(&out);
        printf("%s", out.out.c_str());
        CHECK(contains(out.out, "Kept") && contains(out.out, "  1 s1.example") &&
              contains(out.out, "  3 s3.example"));
    }
    CHECK(fake[2].polls >= 4 && fake[3].polls >= 4);

    printf("A falseticker\n");
    fake[3].bias_us = 50000; // 50 ms out
    run_s(2100);
    table = sources();
    CHECK(contains(table, "x 3 s3.example") && contains(table, "* 1 s1.example"));
    // s1 and s2 combined: (300 - 200) / 2, slewed to over a few polls.
    run_s(4000);
    printf("  clock error %+lld us, combined from 2 at +300, -200\n", (long long)error_us());
    // Each sample is slewed in over two polls, so the clock is caught
    // part-way to it.
    CHECK(!ntp_servers_disagree() && magnitude(error_us() - 50) < 75);
    CHECK(contains(console, "NTP: combining 1, 2\n"));
    // Events are led by the clock's UTC time of day, and the servers in use
    // are those combined.
    CHECK(contains(console, "Z NTP: combining 1, 2\n") && !contains(console, "--:--:--Z NTP: combining"));
    CHECK(ntp_servers_in_use() == 0x06); // servers 1 and 2 (bit 0 is DHCP's)
    {
        Stream out;
        ntp_print(&out, false);
        CHECK(contains(out.out, "3 servers, 1 FALSETICKER"));
    }

    {
        Stream out;
        ntp_print_tracking(&out);
        CHECK(contains(out.out, "Interval:  1024 s (adaptive, at most 1024 s)"));
    }

    printf("The server followed goes quiet: the other that agrees takes over\n");
    fake[1].answers = false;
    console.clear();
    run_s(2100);
    table = sources();
    CHECK(contains(table, "? 1 s1.example") && contains(table, "* 2 s2.example"));
    CHECK(contains(console, "NTP: poll of s1.example failed (no reply )") &&
          contains(console, "NTP: following s2.example (192.0.2.2)") &&
          contains(console, "NTP: servers DISAGREE, no majority"));
    // The clock follows s2 now, 200 us early, slewing gently.
    run_s(4000);
    CHECK(magnitude(error_us() + 200) < 100);
    CHECK(esp_clock_status()->steps == 1);

    printf("Two left, disagreeing: keep the one in use, and say so\n");
    console.clear();
    run_s(2100);
    CHECK(ntp_servers_disagree()); // since s1 went: said so then
    table = sources();
    CHECK(contains(table, "* 2 s2.example") && contains(table, "x 3 s3.example") &&
          contains(table, "Servers DISAGREE"));
    {
        Stream out;
        ntp_print(&out, false);
        CHECK(contains(out.out, "Server:   s2.example (192.0.2.2), stratum 2") &&
              contains(out.out, "DISAGREE"));
    }

    printf("A server changed: started again, and they agree\n");
    console.clear();
    user_prefs_set_string(user_prefs_ntp_server(2), "s4.example");
    fake[4].bias_us = 100;
    run_s(5);
    table = sources();
    // One sample: not selectable yet, so s2 is the only usable server.
    CHECK(contains(table, "? 3 s4.example") && !ntp_servers_disagree());
    CHECK(contains(console, "NTP: servers agree"));
    user_prefs_set_string(user_prefs_ntp_server(1), "");
    run_s(2);
    // s4 alone, without a line: none followed until it has one.
    CHECK(!contains(sources(), "* 3 s4.example"));
    run_s(2100);
    CHECK(!contains(sources(), "s2.example") && contains(sources(), "* 3 s4.example"));
    {
        Stream out;
        ntp_print(&out, true);
        CHECK(contains(out.out, "  2 -\n") && contains(out.out, "Jitter") &&
              contains(out.out,
                       "Server:    3 s4.example (192.0.2.4), stratum 2; 2 servers, 1 UNUSABLE"));
    }

    printf("An address for a name: shown once\n");
    for (int index = 1; index < NTP_SERVERS; index++)
    {
        user_prefs_set_string(user_prefs_ntp_server(index), "");
    }
    user_prefs_set_string(user_prefs.fields.ntp.server, "192.0.2.6");
    console.clear();
    ntp_init();
    run_s(3);
    CHECK(contains(console, "Z NTP: clock set from 192.0.2.6\n"));
    {
        Stream out;
        ntp_print(&out, false);
        CHECK(contains(out.out, "Server:   192.0.2.6, stratum 2, updated"));
    }

    printf("A pool name: resolved once, kept, and again only once it fails\n");
    fake[7] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    fake[8] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    fake[6].answers = true;
    user_prefs_set_string(user_prefs_ntp_server(1), "pool.example");
    pool_lookups = 0;
    console.clear();
    run_s(4000);
    {
        Stream out;
        ntp_print(&out, true);
        CHECK(pool_lookups == 1 && fake[7].polls > 5 && fake[8].polls == 0);
        CHECK(contains(out.out, "resolved 1 times, 0 DNS failures, statistics restarted 0 times"));
        CHECK(!contains(sources(), "? 2 pool.example"));
    }
    // Its address stops answering: after two failed polls, resolved again,
    // and the new address polled.
    fake[7].answers = false;
    console.clear();
    run_s(3000);
    CHECK(pool_lookups == 2 && fake[8].polls > 0);
    CHECK(contains(console, "NTP: poll of pool.example failed"));

    printf("Server 0, DHCP's: polled once offered, preferred, disabled\n");
    fake[9] = (FakeServer){true, 0, 1500, 0, NULL, 0, 0};
    user_prefs_ntp_dhcp = NTP_DHCP_ENABLED;
    run_s(300);
    CHECK(fake[9].polls == 0 && contains(sources(), "? 0 dhcp")); // nothing offered yet
    dhcp_address = IPAddress(IPv4, 9);
    dhcp_offered = true;
    console.clear();
    run_s(3000);
    CHECK(fake[9].polls > 5 && contains(sources(), " 0 dhcp"));
    CHECK(!contains(sources(), "? 0 dhcp") && !contains(sources(), "x 0 dhcp"));
    user_prefs_ntp_dhcp = NTP_DHCP_PREFERRED;
    console.clear();
    run_s(600);
    CHECK(contains(sources(), "* 0 dhcp") && ntp_servers_in_use() == 0x01);
    CHECK(contains(console, "NTP: following dhcp (192.0.2.9)"));
    user_prefs_ntp_dhcp = NTP_DHCP_DISABLED;
    run_s(600);
    CHECK(!contains(sources(), " 0 dhcp") && (ntp_servers_in_use() & 0x01) == 0);
    dhcp_offered = false;

    printf("ntp.asymmetry: every sample moved, the clock with them, the lines kept\n");
    {
        Stream out;
        int64_t before;

        run_s(2000);
        before = error_us();
        CHECK(ntp_set_asymmetry(300) && user_prefs_ntp_asymmetry_us == 300);
        CHECK(!ntp_set_asymmetry(NTP_ASYMMETRY_MAX_US + 1) && user_prefs_ntp_asymmetry_us == 300);
        run_s(3000);
        printf("  clock error %+lld us before, %+lld us after +300\n", (long long)before,
               (long long)error_us());
        CHECK(magnitude(error_us() - (before + 300)) < 75);
        ntp_print(&out, true);
        CHECK(!contains(out.out, "statistics restarted 1 times") ||
              contains(out.out, "pool.example")); // only the pool's earlier restart
        ntp_set_asymmetry(0);
    }

    printf("ntp offset: each server's own samples moved, the clock with them\n");
    {
        int64_t before;

        run_s(6000); // the slew back from ntp.asymmetry's test
        before = error_us();
        for (int server = 0; server <= NTP_SERVERS; server++)
        {
            CHECK(ntp_set_offset(server, 300) && user_prefs_ntp_offset_us[server] == 300);
        }
        CHECK(!ntp_set_offset(1, NTP_OFFSET_MAX_US + 1) && user_prefs_ntp_offset_us[1] == 300);
        CHECK(!ntp_set_offset(NTP_SERVERS + 1, 0));
        ntp_set_offset(2, -120);
        Stream out;
        ntp_print_tracking(&out);
        CHECK(contains(out.out, "Asymmetry: +0 us, server 0 +300 us, server 1 +300 us, server 2 -120 us"));
        ntp_set_offset(2, 300);
        run_s(6000);
        printf("  clock error %+lld us before, %+lld us after +300 on each\n", (long long)before,
               (long long)error_us());
        CHECK(magnitude(error_us() - (before + 300)) < 75);
        for (int server = 0; server <= NTP_SERVERS; server++)
        {
            ntp_set_offset(server, 0);
        }
    }

    printf("A followed server's statistics restart (another address): the clock's window too\n");
    for (int index = 1; index < NTP_SERVERS; index++)
    {
        user_prefs_set_string(user_prefs_ntp_server(index), "");
    }
    user_prefs_set_string(user_prefs.fields.ntp.server, "pool.example");
    fake[7] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    fake[8] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    pool_lookups = 0;
    ntp_init();
    run_s(3000);
    CHECK(esp_clock_window_samples() == ESP_CLOCK_WINDOW);
    fake[7].answers = false; // its first address, kept until it fails
    console.clear();
    run_s(400);
    {
        Stream out;
        ntp_print(&out, true);
        CHECK(pool_lookups == 2 && fake[8].polls > 0);
        CHECK(contains(out.out, "statistics restarted 1 times"));
        printf("  window: %u samples after the new address's first\n", esp_clock_window_samples());
        CHECK(esp_clock_window_samples() < ESP_CLOCK_WINDOW / 2);
    }

    printf("The same address under two numbers: the higher set aside, not polled\n");
    fake[5] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    fake[6] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
    user_prefs_set_string(user_prefs.fields.ntp.server, "192.0.2.6");
    user_prefs_set_string(user_prefs_ntp_server(1), "s6.example");
    ntp_init();
    console.clear();
    run_s(1500);
    {
        std::string table = sources();
        CHECK(contains(table, "= 2 s6.example") && contains(table, "192.0.2.6, as server 1\n"));
        CHECK(contains(console, "is the same address as server 1: not polled"));
        CHECK(contains(table, "* 1 192.0.2.6") && (ntp_servers_in_use() & 0x04) == 0);
    }
    uint32_t polls_6 = fake[6].polls;
    run_s(1280);
    printf("  192.0.2.6 polled %u times in 1280 s, as one server\n", fake[6].polls - polls_6);
    CHECK(fake[6].polls - polls_6 <= 1280 / 128 + 1);
    // Another host for it: polled again.
    user_prefs_set_string(user_prefs_ntp_server(1), "s5.example");
    run_s(1500);
    CHECK(!contains(sources(), "= 2") && contains(sources(), " 2 s5.example") && fake[5].polls >= 3);

    printf("DHCP's server the same as one set by name: DHCP's (0) kept\n");
    dhcp_address = IPAddress(IPv4, 5);
    dhcp_offered = true;
    user_prefs_ntp_dhcp = NTP_DHCP_ENABLED;
    user_prefs_ntp_offset_us[2] = 120; // set for s5.example, not for DHCP's
    console.clear();
    run_s(1500);
    CHECK(contains(sources(), "= 2 s5.example") &&
          contains(sources(), ", as server 0 (its offset +0 us used, not +120)\n"));
    CHECK(contains(console, "server 2's offset +120 us is not used: server 0's (+0 us) is"));
    user_prefs_ntp_offset_us[2] = 0;
    CHECK(!contains(sources(), "= 0"));
    user_prefs_ntp_dhcp = NTP_DHCP_DISABLED;
    dhcp_offered = false;
    console.clear();
    run_s(1500);
    CHECK(!contains(sources(), "= 2") &&
          contains(console, "server 2 is no longer the same address as server 0"));

    printf("Two usable servers 20 ms apart: DISAGREE, and the bound covers both\n");
    {
        SML_TIME_NTP_PAYLOAD report;
        int under = 0;

        fake[5].bias_us = 20000;
        pps_on = true;
        run_s(3000);
        CHECK(ntp_servers_disagree());
        CHECK(ntp_take_report(&report));
        printf("  bound %.3f ms, clock error %+lld us\n", report.uncertainty_ns / 1e6,
               (long long)error_us());
        CHECK(report.uncertainty_ns > 20000000U - 1000000U);
        CHECK(report.uncertainty_ns > (uint32_t)(magnitude(error_us()) * 1000));
        // The one followed moves 6 ms (gently slewed, minutes): the bound,
        // worked out each second, holds every second meanwhile.
        int followed = contains(sources(), "* 1 ") ? 6 : 5;
        fake[followed].bias_us += 6000;
        for (int second = 0; second < 1500; second++)
        {
            run_s(1);
            if (ntp_take_report(&report) &&
                report.uncertainty_ns < (uint32_t)(magnitude(error_us() + (followed == 6 ? 0 : 0)) * 1000))
            {
                under++;
            }
        }
        printf("  slewing 6 ms: bound under the true error in %d of 1500 seconds\n", under);
        CHECK(under == 0 && ntp_servers_disagree());
        fake[followed].bias_us -= 6000;
        run_s(2000);
        // TIME-NTP's last update: the sample's own UTC, the true time then.
        CHECK(ntp_take_report(&report) &&
              report.last_update_utc ==
                  (uint32_t)((esp_clock_status()->last_update_local_us + TRUTH_US +
                              fake[followed].bias_us) / 1000000));
        // Agreeing again: the bound back to the clock's own.
        fake[5].bias_us = 0;
        run_s(4000);
        CHECK(!ntp_servers_disagree());
        CHECK(ntp_take_report(&report) && report.uncertainty_ns < 5000000U);
        printf("  agreeing: bound %.3f ms\n", report.uncertainty_ns / 1e6);
    }

    printf("From start: ACQUIRING and the crystal's drift allowance until the line is full\n");
    {
        SML_TIME_NTP_PAYLOAD report;

        user_prefs_set_string(user_prefs_ntp_server(1), "");
        ntp_init();
        run_s(10);
        CHECK(ntp_take_report(&report) && report.lock_state == SML_LOCK_ACQUIRING);
        CHECK(esp_clock_drift_ppb() == 50000); // no frequency yet
        for (int waited = 0; waited < 1000 && esp_clock_status()->frequency_samples == 0; waited += 10)
        {
            run_s(10);
        }
        // A first estimate (the cap on a young line's skew: test_esp_clock).
        CHECK(esp_clock_status()->frequency_samples != 0);
        CHECK(esp_clock_drift_ppb() <= ESP_CLOCK_DRIFT_YOUNG_PPB);
        CHECK(ntp_take_report(&report) && report.lock_state == SML_LOCK_ACQUIRING);
        run_s(3000);
        CHECK(ntp_take_report(&report) && report.lock_state == SML_LOCK_LOCKED);
        CHECK(esp_clock_drift_ppb() < 2000);
        pps_on = false;
    }

    printf("Kiss-o'-death RATE: that server polled half as often, then eased back\n");
    {
        Stream out;

        user_prefs_set_string(user_prefs_ntp_server(1), "s5.example");
        fake[5] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
        fake[6] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
        ntp_init();
        run_s(3000);
        uint32_t before = fake[5].polls;
        fake[5].kiss = "RATE";
        console.clear();
        run_s(600);
        CHECK(contains(console, "s5.example asks to be polled less often (RATE): every "));
        fake[5].kiss = NULL;
        uint32_t at = fake[5].polls;
        uint32_t at_6 = fake[6].polls;
        run_s(4096);
        printf("  after RATE: server 2 polled %u times in 4096 s, server 1 %u times\n",
               fake[5].polls - at, fake[6].polls - at_6);
        CHECK(fake[5].polls - at < fake[6].polls - at_6 && at > before);
        ntp_print(&out, true);
        CHECK(contains(out.out, "RATE, 0 DENY or RSTR, 0 other") && contains(out.out, "(RATE)"));
        run_s(512 * 100); // 8 good polls at each slower rate, from the longest down
        out.out.clear();
        ntp_print(&out, true);
        CHECK(!contains(out.out, "(RATE)")); // eased back after good polls
    }

    printf("Kiss-o'-death DENY: not polled again until ntp sync\n");
    {
        Stream out;

        fake[5].kiss = "DENY";
        console.clear();
        run_s(1100);
        CHECK(contains(console, "s5.example refuses to serve (DENY)"));
        uint32_t at = fake[5].polls;
        run_s(10000);
        CHECK(fake[5].polls == at);
        ntp_print(&out, true);
        CHECK(contains(out.out, "REFUSED (DENY), not polled") && contains(out.out, "1 DENY or RSTR"));
        CHECK(contains(sources(), "? 2 s5.example"));
        fake[5].kiss = NULL;
        ntp_resync();
        run_s(30);
        CHECK(fake[5].polls > at);
        out.out.clear();
        ntp_print(&out, true);
        CHECK(!contains(out.out, "REFUSED"));
    }

    printf("ntp.interval lowered: polls already scheduled come in, in order\n");
    {
        user_prefs.fields.ntp.interval = 86400;
        ntp_init();
        run_s(600);
        fake[5].kiss = "RATE"; // a long wait for server 2: 1024 s and up
        run_s(1100);
        fake[5].kiss = NULL;
        fake[5].kiss = "RATE";
        run_s(2200);
        fake[5].kiss = NULL;
        Stream out;
        ntp_print(&out, true);
        uint32_t at = fake[5].polls;
        user_prefs.fields.ntp.interval = 128;
        run_s(128 * 4 + 64);
        printf("  after lowering to 128 s: %u polls of server 2 in 576 s\n", fake[5].polls - at);
        CHECK(fake[5].polls > at);
        user_prefs.fields.ntp.interval = 512;
    }

    printf("Unbounded (4.29 s and more): UNCERTAINTY_VALID cleared, not a 4.29 s bound\n");
    {
        SML_TIME_NTP_PAYLOAD report;

        fake[5] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
        fake[6] = (FakeServer){true, 5000000, 2000, 0, NULL, 0, 0}; // 5 s off
        user_prefs_set_string(user_prefs_ntp_server(1), "s6.example");
        ntp_init();
        user_prefs_set_string(user_prefs_ntp_server(1), "s5.example");
        user_prefs_set_string(user_prefs_ntp_server(2), "s6.example");
        pps_on = true;
        run_s(4000);
        CHECK(ntp_servers_disagree());
        CHECK(ntp_take_report(&report) && report.uncertainty_ns == UINT32_MAX &&
              (report.flags & SML_NTP_UNCERTAINTY_VALID) == 0 &&
              (report.flags & SML_NTP_TIME_VALID) != 0);
        Stream out;
        ntp_print(&out, false);
        CHECK(contains(out.out, "+/- over 4.29 s"));
        pps_on = false;
        user_prefs_set_string(user_prefs_ntp_server(2), "");
    }

    printf("Short polls while the board temperature moves, in at once\n");
    {
        user_prefs_esp_tcomp.learnt_ppb_per_c = -190; // learnt: no start's short polls
        user_prefs_esp_tcomp.learnt_estimates = 3;
        user_prefs.fields.ntp.interval = 1024;
                ntp_init();
        run_s(4 * 3600);
        Stream before;
        ntp_print_tracking(&before);
        CHECK(!contains(before.out, "Interval:  128 s") && !contains(before.out, "short:"));
        console.clear();
        uint32_t at = all_polls();
        // 1 C over 20 min, then steady.
        for (int t = 0; t < 1200; t += 5)
        {
            ntp_board_temperature(3000 + t * 100 / 1200);
            run_s(5);
        }
        Stream during;
        ntp_print_tracking(&during);
        printf("  %u polls in the 20 min of the move\n", all_polls() - at);
        // Learnt (3 estimates): 256 s while it moves.
        CHECK(contains(console, "NTP: short polls: temperature moving, 256 s\n"));
        // At most 256 s: the adaptation may still go shorter.
        CHECK((contains(during.out, "Interval:  256 s ") || contains(during.out, "Interval:  128 s ")) &&
              contains(during.out, "(adaptive, at most 1024 s, short: temperature moving, 256 s)"));
        CHECK(all_polls() - at >= 4);
        for (int t = 0; t < 4500; t += 5)
        {
            ntp_board_temperature(3100);
            run_s(5);
        }
        CHECK(contains(console, "NTP: short polls end\n"));
        at = all_polls();
        for (int t = 0; t < 7200; t += 5)
        {
            ntp_board_temperature(3100);
            run_s(5);
        }
        printf("  %u polls in the 2 h after\n", all_polls() - at);
        CHECK(all_polls() - at < 2 * 3600 / 128);

        printf("  Not learnt yet: 128 s\n");
        user_prefs_esp_tcomp.learnt_estimates = 1;
        ntp_init();
        run_s(4 * 3600);
        console.clear();
        for (int t = 0; t < 1200; t += 5)
        {
            ntp_board_temperature(3000 + t * 100 / 1200);
            run_s(5);
        }
        Stream learning;
        ntp_print_tracking(&learning);
        CHECK(contains(console, "NTP: short polls: learning, 128 s\n"));
        CHECK(contains(learning.out, "Interval:  128 s (adaptive, at most 1024 s, short: learning, 128 s)"));
        user_prefs_esp_tcomp.learnt_ppb_per_c = 0;
        user_prefs_esp_tcomp.learnt_estimates = 0;
    }

    printf("Identity from a reply: stratum, refid, root delay, reference time\n");
    {
        uint8_t packet[NTP_PACKET_BYTES];
        uint64_t cookie = 0x0123456789ABCDEFULL;
        NtpReply reply;
        NtpReply other;

        memset(packet, 0, sizeof packet);
        packet[0] = (0U << 6) | (4U << 3) | 4U;
        packet[1] = 1;
        packet[7] = 0x42;                         // root delay
        memcpy(packet + 12, "GPS", 3);            // refid
        uint64_t reference = (3973000000ULL << 32) | 0x80000000ULL;
        uint64_t stamps[4] = {reference, cookie, (3973000100ULL << 32) | 1, (3973000100ULL << 32) | 2};
        for (int field = 0; field < 4; field++)
        {
            for (int byte = 0; byte < 8; byte++)
            {
                packet[16 + 8 * field + byte] = (uint8_t)(stamps[field] >> (56 - 8 * byte));
            }
        }
        CHECK(ntp_packet_parse(packet, sizeof packet, cookie, &reply) == NTP_REPLY_OK);
        CHECK(reply.identity.valid && reply.identity.stratum == 1 &&
              reply.identity.refid == 0x47505300U && reply.identity.root_delay == 0x42 &&
              reply.identity.reference == reference);
        CHECK(ntp_packet_parse(packet, sizeof packet, cookie, &other) == NTP_REPLY_OK &&
              ntp_identity_same(&reply.identity, &other.identity));
        other.identity.refid = 0x50505300U; // PPS
        CHECK(!ntp_identity_same(&reply.identity, &other.identity));
        other = reply;
        other.identity.reference++;
        CHECK(!ntp_identity_same(&reply.identity, &other.identity));
        // A reference time of whole seconds: never used.
        packet[20] = packet[21] = packet[22] = packet[23] = 0;
        CHECK(ntp_packet_parse(packet, sizeof packet, cookie, &other) == NTP_REPLY_OK &&
              !other.identity.valid && !ntp_identity_same(&other.identity, &other.identity));
    }

    printf("One host at two addresses: the higher number set aside once its replies match\n");
    {
        for (int n = 0; n < 10; n++)
        {
            fake[n] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
        }
        fake[5].host = fake[6].host = 1; // one machine, at .5 and .6
        fake[7].host = 2;                // another, alike in all but its reference time
        user_prefs_ntp_dhcp = NTP_DHCP_DISABLED;
        user_prefs_set_string(user_prefs.fields.ntp.server, "s5.example");
        user_prefs_set_string(user_prefs_ntp_server(1), "s6.example");
        user_prefs_set_string(user_prefs_ntp_server(2), "s7.example");
        user_prefs_set_string(user_prefs_ntp_server(3), "");
        ntp_init();
        console.clear();
        run_s(4000);
        table = sources();
        CHECK(contains(table, "= 2 s6.example") &&
              contains(table, "192.0.2.6, same host as server 1\n"));
        CHECK(contains(console, "is the same host as server 1 (its replies match): not used"));
        CHECK(!contains(table, "= 3") && !contains(table, "= 1"));
        CHECK((ntp_servers_in_use() & 0x04) == 0);
        uint32_t polls_6 = fake[6].polls;
        run_s(7 * 3600);
        printf("  192.0.2.6 polled %u times in 7 h, as checks\n", fake[6].polls - polls_6);
        CHECK(fake[6].polls - polls_6 >= 1 && fake[6].polls - polls_6 <= 2);
        CHECK(contains(sources(), "= 2 s6.example"));
        Stream verbose;
        ntp_print_sources(&verbose, true);
        CHECK(contains(verbose.out, "; same host as server 1, checked "));

        printf("  A resync and the network coming back: checks out of turn do not count\n");
        for (int n = 0; n < 4; n++)
        {
            ntp_service(false);
            run_s(30);
            ntp_resync();
            run_s(30);
        }
        CHECK(contains(sources(), "= 2 s6.example") && !contains(console, "no longer taken"));

        printf("  No longer the same host: polled again after its checks stop matching\n");
        fake[6].host = 3;
        console.clear();
        run_s(4 * 6 * 3600);
        CHECK(contains(console, "server 2 is no longer taken for the same host as server 1: not "
                                "matched at its last checks"));
        CHECK(!contains(sources(), "= 2"));

        printf("  The kept server unreachable: the other polled again, and followed\n");
        fake[6].host = 1;
        run_s(4000);
        CHECK(contains(sources(), "= 2 s6.example"));
        fake[5].answers = false;
        fake[7].answers = false;
        console.clear();
        run_s(3 * 3600);
        CHECK(contains(console, "server 2 is no longer taken for the same host as server 1: that "
                                "one is not reachable"));
        CHECK(contains(sources(), "* 2 s6.example"));

        printf("  A host updating its reference time every second: never matched\n");
        for (int n = 5; n <= 7; n++)
        {
            fake[n] = (FakeServer){true, 0, 2000, 0, NULL, 0, 0};
        }
        fake[5].host = fake[6].host = 1;
        fake[5].reference_period_s = fake[6].reference_period_s = 1;
        ntp_init();
        console.clear();
        run_s(4 * 3600);
        CHECK(!contains(sources(), "= 2") && !contains(console, "same host"));

        printf("  A pool name set aside, given another address when looked up again: polled\n");
        fake[9] = (FakeServer){true, 0, 2000, 0, NULL, 1, 1024};
        fake[7] = (FakeServer){true, 0, 2000, 0, NULL, 1, 1024}; // the pool's first: the same host
        fake[8] = (FakeServer){true, 0, 2000, 0, NULL, 2, 1024}; // its second: another
        user_prefs_set_string(user_prefs.fields.ntp.server, "s9.example");
        user_prefs_set_string(user_prefs_ntp_server(1), "pool.example");
        user_prefs_set_string(user_prefs_ntp_server(2), "");
        pool_lookups = 0;
        ntp_init();
        run_s(4000);
        CHECK(contains(sources(), "= 2 pool.example") &&
              contains(sources(), "192.0.2.7, same host as server 1\n"));
        console.clear();
        ntp_service(false); // the network gone and back: names looked up again
        run_s(30);
        CHECK(contains(console, "server 2 is no longer taken for the same host as server 1: its "
                                "name now gives another address"));
        run_s(4000);
        CHECK(!contains(sources(), "= 2") && contains(sources(), "192.0.2.8"));
        printf("  The kept server a pool name given another address: the other polled again\n");
        user_prefs_set_string(user_prefs.fields.ntp.server, "pool.example");
        user_prefs_set_string(user_prefs_ntp_server(1), "s9.example");
        pool_lookups = 0;
        ntp_init();
        run_s(4000);
        CHECK(contains(sources(), "= 2 s9.example") &&
              contains(sources(), "192.0.2.9, same host as server 1\n"));
        console.clear();
        ntp_service(false);
        run_s(30);
        CHECK(contains(console, "server 2 is no longer taken for the same host as server 1: that "
                                "one now has another address"));
        CHECK(!contains(sources(), "= 2"));
        user_prefs_set_string(user_prefs.fields.ntp.server, "s5.example");
        user_prefs_set_string(user_prefs_ntp_server(1), "s6.example");
        fake[5].reference_period_s = fake[6].reference_period_s = 1024;
        ntp_init();
        run_s(4000);

        printf("  Three servers one host: both higher numbers set aside, as server 1's\n");
        for (int n = 5; n <= 8; n++)
        {
            fake[n] = (FakeServer){true, 0, 2000, 0, NULL, 1, 1024};
        }
        fake[8].host = 2;
        user_prefs_set_string(user_prefs.fields.ntp.server, "s5.example");
        user_prefs_set_string(user_prefs_ntp_server(1), "s6.example");
        user_prefs_set_string(user_prefs_ntp_server(2), "s7.example");
        user_prefs_set_string(user_prefs_ntp_server(3), "s8.example");
        ntp_init();
        console.clear();
        run_s(6000);
        table = sources();
        printf("%s", table.c_str());
        CHECK(contains(table, "192.0.2.6, same host as server 1\n") &&
              contains(table, "192.0.2.7, same host as server 1\n"));
        CHECK(!contains(table, "= 4") && !contains(table, "= 1"));
        CHECK((ntp_servers_in_use() & 0x0C) == 0);
        user_prefs_set_string(user_prefs_ntp_server(2), "");
        user_prefs_set_string(user_prefs_ntp_server(3), "");
        run_s(5);

        printf("  Server 1 changed: the one set aside as its host polled again\n");
        CHECK(contains(sources(), "= 2 s6.example"));
        console.clear();
        user_prefs_set_string(user_prefs.fields.ntp.server, "s7.example");
        run_s(5);
        CHECK(contains(console, "server 2 is no longer taken for the same host as server 1: it has "
                                "changed"));
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
