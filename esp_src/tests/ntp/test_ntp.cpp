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
    if (server->answers)
    {
        result.ok = true;
        result.replies = 4;
        result.sample.local_us = fake_now_us;
        result.sample.offset_us = TRUTH_US + server->bias_us;
        result.sample.delay_us = server->delay_us;
        result.sample.root_delay_us = 1000;
        result.sample.root_dispersion_us = 500;
        result.sample.stratum = 2;
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


bool pps_output_take(PpsEdge *)
{
    return false;
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
    fake[7] = (FakeServer){true, 0, 2000, 0};
    fake[8] = (FakeServer){true, 0, 2000, 0};
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
    fake[9] = (FakeServer){true, 0, 1500, 0};
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

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
