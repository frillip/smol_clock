/*
 * How long loop() takes: the longest pass since start, which service was
 * slowest in it and when, each service's longest, and how many passes ran
 * over a second (the ESP PPS runs on by itself through those: "ran on" in
 * ntp -v). For sizing the task watchdog, and for finding what blocks.
 *
 * loop() marks the start of a pass, notes each service as it returns, and
 * marks the end. Portable: times are local microseconds passed in.
 */
#ifndef LOOP_TIMING_H
#define LOOP_TIMING_H

#include <stdbool.h>
#include <stdint.h>

#define LOOP_TIMING_SERVICES 16 // services noted: more are counted as "other"
#define LOOP_TIMING_SLOW_US  1000000LL
#define LOOP_TIMING_DETAIL   24 // what a service was doing: a command, say

typedef struct
{
    const char *name; // as noted: a string that lives as long as the program
    int64_t longest_us;
    char longest_detail[LOOP_TIMING_DETAIL]; // what it was doing then, or ""
    uint32_t slow; // times over LOOP_TIMING_SLOW_US
} LoopService;

typedef struct
{
    uint32_t passes;
    uint32_t slow_passes; // over LOOP_TIMING_SLOW_US
    int64_t longest_us;   // the longest pass
    const char *longest_by;
    char longest_detail[LOOP_TIMING_DETAIL]; // what that service was doing, or ""
    int64_t longest_by_us;   // that service's share of it
    int64_t longest_at_us;   // its end, local time
    bool longest_utc_known;  // ... and as UTC, if known then
    uint32_t longest_utc;
} LoopTiming;

void loop_timing_start(int64_t now_us);
// What the service about to be noted is doing (a console's command line,
// cut to fit): the service that dispatched a command is the one timed, so
// without it "telnet 19 s" says nothing of the update check it ran.
void loop_timing_detail(const char *text);
// service has just returned.
void loop_timing_note(const char *service, int64_t now_us);
void loop_timing_end(int64_t now_us, bool utc_known, uint32_t utc_seconds);

const LoopTiming *loop_timing(void);
// The services noted, in the order first seen; *count of them.
const LoopService *loop_timing_services(int *count);

#endif // LOOP_TIMING_H
