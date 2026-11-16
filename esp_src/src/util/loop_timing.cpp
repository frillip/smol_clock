#include "loop_timing.h"

#include <string.h>

static LoopTiming timing;
static LoopService services[LOOP_TIMING_SERVICES];
static int service_count;
static int64_t pass_start_us;
static int64_t mark_us;
static const char *pass_slowest;
static int64_t pass_slowest_us;
static char pass_slowest_detail[LOOP_TIMING_DETAIL];
static char pending_detail[LOOP_TIMING_DETAIL]; // for the next service noted


static void copy_detail(char *destination, const char *text)
{
    size_t length = strlen(text);

    if (length >= LOOP_TIMING_DETAIL)
    {
        length = LOOP_TIMING_DETAIL - 1;
    }
    memcpy(destination, text, length);
    destination[length] = '\0';
}


static LoopService *find(const char *name)
{
    for (int index = 0; index < service_count; index++)
    {
        if (services[index].name == name || strcmp(services[index].name, name) == 0)
        {
            return &services[index];
        }
    }
    if (service_count < LOOP_TIMING_SERVICES)
    {
        services[service_count].name = name;
        return &services[service_count++];
    }
    return &services[LOOP_TIMING_SERVICES - 1]; // the last one stands for the rest
}


void loop_timing_start(int64_t now_us)
{
    pass_start_us = now_us;
    mark_us = now_us;
    pass_slowest = NULL;
    pass_slowest_us = -1;
    pass_slowest_detail[0] = '\0';
    pending_detail[0] = '\0';
}


void loop_timing_detail(const char *text)
{
    copy_detail(pending_detail, text != NULL ? text : "");
}


void loop_timing_note(const char *service, int64_t now_us)
{
    int64_t took = now_us - mark_us;
    LoopService *entry = find(service);

    mark_us = now_us;
    if (took > entry->longest_us)
    {
        entry->longest_us = took;
        copy_detail(entry->longest_detail, pending_detail);
    }
    if (took > LOOP_TIMING_SLOW_US)
    {
        entry->slow++;
    }
    if (took > pass_slowest_us)
    {
        pass_slowest = entry->name;
        pass_slowest_us = took;
        copy_detail(pass_slowest_detail, pending_detail);
    }
    pending_detail[0] = '\0';
}


void loop_timing_end(int64_t now_us, bool utc_known, uint32_t utc_seconds)
{
    int64_t took = now_us - pass_start_us;

    if (now_us > mark_us)
    {
        loop_timing_note("loop", now_us); // between the last service and the end
    }
    timing.passes++;
    if (took > LOOP_TIMING_SLOW_US)
    {
        timing.slow_passes++;
    }
    if (took > timing.longest_us)
    {
        timing.longest_us = took;
        timing.longest_by = pass_slowest;
        copy_detail(timing.longest_detail, pass_slowest_detail);
        timing.longest_by_us = pass_slowest_us;
        timing.longest_at_us = now_us;
        timing.longest_utc_known = utc_known;
        timing.longest_utc = utc_seconds;
    }
}


const LoopTiming *loop_timing(void)
{
    return &timing;
}


const LoopService *loop_timing_services(int *count)
{
    *count = service_count;
    return services;
}
