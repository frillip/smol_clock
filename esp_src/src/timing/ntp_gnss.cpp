#include "ntp_gnss.h"

#include "../pic/pic_link.h"
#include "../util/time_text.h"
#include "ntp.h"
#include "phase_stats.h"

#include <string.h>

extern "C"
{
#include "../pic/sml/sml.h"
}

static PhaseStats stats;
static int64_t last_edge_us;
static int64_t latest_ns;
static bool have_latest;

void ntp_gnss_service(void)
{
    PicTime time;
    int64_t ntp_utc_us;

    if (!pic_link_time(&time) || time.edge_local_us == last_edge_us)
    {
        return;
    }
    last_edge_us = time.edge_local_us;
    if (time.source != SML_SOURCE_GNSS || time.lock_state != SML_LOCK_LOCKED ||
        !time.uncertainty_valid || time.uncertainty_ns > NTP_GNSS_MAX_NS ||
        !ntp_is_synced() || !ntp_utc_at(time.edge_local_us, &ntp_utc_us))
    {
        return;
    }
    latest_ns = (ntp_utc_us - time.edge_utc_us) * 1000;
    have_latest = true;
    phase_stats_add_tagged(&stats, (uint32_t)(time.edge_utc_us / 1000000), latest_ns,
                           ntp_servers_in_use());
}


void ntp_gnss_reset(void)
{
    phase_stats_reset(&stats);
    have_latest = false;
}


static void print_sum(Stream *output, const char *label, const PhaseSum *sum)
{
    output->printf("  %-10s mean ", label);
    time_print_ns(output, (int64_t)phase_sum_mean(sum));
    output->printf(", sd ");
    time_print_ns(output, (int64_t)phase_sum_sd(sum));
    output->printf(", RMS ");
    time_print_ns(output, (int64_t)phase_sum_rms(sum));
    output->printf(", ");
    time_print_ns(output, sum->minimum);
    output->printf(" to ");
    time_print_ns(output, sum->maximum);
    output->printf(" (%lu s)\n", (unsigned long)sum->count);
}


// The servers a minute's tag names ("34": servers 3 and 4, 0 DHCP's), "*" after them
// if they changed within the minute.
static void format_servers(char *text, uint8_t tag, bool varied)
{
    char *at = text;

    for (int index = 0; index < 6; index++)
    {
        if (tag & (1U << index))
        {
            *at++ = (char)('0' + index);
        }
    }
    if (at == text)
    {
        strcpy(text, "none"); // coasting
        at = text + 4;
    }
    if (varied)
    {
        *at++ = '*';
    }
    *at = '\0';
}


// The last 24 hours a minute at a time, each minute's mean in us, oldest
// first, ten to a line, each line led by its first minute's UTC hh:mm. "-"
// for a minute with nothing; lines before the first minute seen left out.
// Under each line, the servers in use (format_servers), where they change
// and at the line's first minute with data, blank where they are the same.
static void print_minutes(Stream *output, uint32_t now_s)
{
    uint32_t span = PHASE_STATS_DAY_MINUTES;
    uint32_t since = now_s / 60U - stats.since_s / 60U + 1U; // minutes seen, to now
    char previous[8] = "";
    uint32_t ago;

    if (since < span)
    {
        span = (since + 9U) / 10U * 10U; // whole lines
    }
    output->printf("  minute means, us (oldest first), and under them the servers in use\n"
                   "  where they change (34: servers 3 and 4, *: changed in the minute):\n");
    for (ago = span; ago >= 10U; ago -= 10U)
    {
        char servers[10][8];
        bool shown = false;
        uint32_t start = (now_s / 60U - (ago - 1U)) * 60U;

        output->printf("  %02lu:%02lu", (unsigned long)(start / 3600U % 24U),
                       (unsigned long)(start / 60U % 60U));
        for (uint32_t column = 0; column < 10U; column++)
        {
            uint32_t minute_ago = ago - 1U - column;
            int32_t mean;
            uint8_t tag;
            bool varied;

            servers[column][0] = '\0';
            if (!phase_stats_minute_mean(&stats, now_s, minute_ago, &mean))
            {
                output->printf(" %6s", "-");
                continue;
            }
            output->printf(" %6.0f", mean / 1000.0);
            if (phase_stats_minute_tag(&stats, now_s, minute_ago, &tag, &varied))
            {
                char text[8];

                format_servers(text, tag, varied);
                if (!shown || strcmp(text, previous) != 0)
                {
                    strcpy(servers[column], text);
                    shown = true;
                }
                strcpy(previous, text);
            }
        }
        output->printf("\n       ");
        for (uint32_t column = 0; column < 10U; column++)
        {
            output->printf(" %6s", servers[column]);
        }
        output->printf("\n");
    }
}


void ntp_gnss_print(Stream *output, bool verbose)
{
    PhaseSum hour;
    uint32_t now_s;

    if (!stats.started)
    {
        output->printf("NTP vs GNSS: nothing yet (needs the PIC locked on GNSS)\n");
        return;
    }
    if (!ntp_utc_now(&now_s))
    {
        now_s = stats.minute * 60U + 59U;
    }
    output->printf("NTP vs GNSS at ");
    time_print_utc(output, now_s);
    output->printf(": latest ");
    time_print_ns(output, have_latest ? latest_ns : 0);
    output->printf(" (+ = NTP ahead)\n");
    phase_stats_hour(&stats, now_s, &hour);
    if (hour.count != 0)
    {
        print_sum(output, "last hour:", &hour);
    }
    output->printf("  since ");
    time_print_utc(output, stats.since_s);
    output->printf(":\n");
    print_sum(output, "", &stats.total);
    if (verbose)
    {
        print_minutes(output, now_s);
    }
}
