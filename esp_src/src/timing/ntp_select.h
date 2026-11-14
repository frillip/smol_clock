/*
 * Choosing the NTP server the clock follows, as chronyd does in outline.
 * Portable, for the host tests.
 *
 * Each usable server says the clock is off by offset, give or take its root
 * distance: the interval [offset - distance, offset + distance] should hold
 * the truth. The largest set of servers whose intervals share a point are
 * the ones that agree; the rest are falsetickers. A tie goes to the set
 * holding the server in use, then to the one holding the lowest-numbered
 * server, so with two servers that disagree the clock keeps to the one it
 * has (or the first) rather than coast with neither.
 *
 * Without a majority (two servers disagreeing, or two pairs) the choice is
 * made all the same, and reported (majority false): the clock follows a
 * server the others do not vouch for.
 *
 * Among those that agree, the one with the shortest distance is followed;
 * the one in use is kept unless another's distance is under half of its.
 *
 * Combining (ntp_combine), as chronyd does (combine_sources): with a
 * majority, the servers that agree, whose distance is within
 * NTP_COMBINE_LIMIT times the least among them (plus chronyd's reselect
 * distance, NTP_COMBINE_MARGIN_US; the least, not the followed one's, see
 * ntp_combine) and whose frequency is within
 * NTP_COMBINE_LIMIT times the sum of the two skews of the followed one's,
 * are averaged: offsets weighted by 1 / distance, frequencies by
 * 1 / frequency sd^2, and the skew is 1 / sqrt(sum of 1 / skew^2). The
 * clock is steered by those rather than the followed server's alone.
 * Without a majority, the followed server alone. A server without a fitted
 * line yet (ntp_stats.h) adds its offset but not its frequency. The servers
 * that agree but are left out are shown '-', as chronyc does.
 *
 * NTP_COMBINE_LIMIT is 1.5 where chronyd's combinelimit is 3: a LAN server
 * is then combined with LAN servers alone, and an Internet server, whose
 * path's asymmetry is hundreds of us, only with Internet servers about as
 * far. With 3 an Internet server came in and out of the LAN one's average,
 * moving the clock by ~200 us each time (seen against GNSS, 2026-11-07).
 * And, as chronyd, a server left out stays out a while: its distant count
 * (kept by the caller) gains NTP_COMBINE_DISTANT_STEP each time it is left
 * out while under NTP_COMBINE_DISTANT, loses one at each combining it
 * would have joined, and it joins only at zero. A server near the limit
 * does not flit in and out. Unlike chronyd, a young server (a short line,
 * its skew still large from Student's t on few degrees of freedom) is not
 * combined at all, and gains no count: its distance is mostly its youth
 * (skew x the time since its best sample), so it fell inside the limit
 * just after each of its polls and outside it a minute later, and the
 * combined set turned over at every poll for the first quarter of an hour
 * (2026-11-09). With a count, it was kept out a quarter of an hour after
 * its line had settled (2026-11-07). The followed server may be young.
 */
#ifndef NTP_SELECT_H
#define NTP_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#define NTP_SELECT_MAX    5 // server 0 (DHCP's) and 1 to 4
#define NTP_COMBINE_LIMIT          1.5
#define NTP_COMBINE_MARGIN_US      100 // chronyd's reselectdist
#define NTP_COMBINE_DISTANT        32  // chronyd's DISTANT_PENALTY
#define NTP_COMBINE_DISTANT_STEP   8   // chronyd's SOURCE_REACH_BITS

typedef enum
{
    NTP_VERDICT_NONE = 0,    // not configured
    NTP_VERDICT_UNUSABLE,    // no recent good sample: ?
    NTP_VERDICT_FALSETICKER, // outside the set that agrees: x
    NTP_VERDICT_CANDIDATE,   // agrees, not followed: +
    NTP_VERDICT_SELECTED,    // followed: *
} NtpVerdict;

typedef struct
{
    bool configured;
    bool usable;
    int64_t offset_us;   // the server's view of our clock's error
    int64_t distance_us; // its root distance, > 0
    // The distance to choose the followed server by, if not distance_us (0):
    // a young line's with its skew capped (ntp.cpp), so the luck of which
    // server was polled last does not choose while every line is short.
    int64_t rank_us;
    // Its fitted line (ntp_stats.h), for combining: fitted false if none.
    bool fitted;
    double frequency_ppb;
    double frequency_sd_ppb;
    double skew_ppb;
    bool young; // its line is short (under NTP_STATS_MIN_SAMPLES): not combined
} NtpCandidate;

typedef struct
{
    int selected;  // the server to follow, -1 if none is usable
    bool majority; // the agreeing set is more than half the usable servers
    int usable;
    int agreeing;
    NtpVerdict verdict[NTP_SELECT_MAX];
    // ntp_combine():
    int combined;                  // servers in the average, 0 with none followed
    bool in_combined[NTP_SELECT_MAX];
    int64_t combined_offset_us;    // the weighted average of their offsets
    bool frequency_known;          // one of them has a fitted line
    double combined_frequency_ppb;
    double combined_skew_ppb;
} NtpSelection;

// current: the server followed now, or -1.
void ntp_select(const NtpCandidate *candidates, int count, int current, NtpSelection *selection);

// After ntp_select(), on the same candidates. distant: each server's count,
// NTP_SELECT_MAX of them, kept between calls (zero for a new server).
void ntp_combine(const NtpCandidate *candidates, int count, uint8_t *distant,
                 NtpSelection *selection);

// *, +, x, ? or a space; combined false turns + into - (agrees, left out).
char ntp_verdict_marker(NtpVerdict verdict, bool combined);

#endif // NTP_SELECT_H
