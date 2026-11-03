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
 * Combining (ntp_combine), as chronyd does: with a majority, the offsets of
 * the servers that agree and whose distance is within NTP_COMBINE_LIMIT
 * times the followed one's are averaged, each weighted by 1 / its distance,
 * and the clock is steered by that average rather than the followed
 * server's offset alone. Without a majority, the followed server alone.
 * The servers that agree but are left out are shown '-', as chronyc does.
 */
#ifndef NTP_SELECT_H
#define NTP_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#define NTP_SELECT_MAX    4
#define NTP_COMBINE_LIMIT 3

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
} NtpSelection;

// current: the server followed now, or -1.
void ntp_select(const NtpCandidate *candidates, int count, int current, NtpSelection *selection);

// After ntp_select(), on the same candidates.
void ntp_combine(const NtpCandidate *candidates, int count, NtpSelection *selection);

// *, +, x, ? or a space; combined false turns + into - (agrees, left out).
char ntp_verdict_marker(NtpVerdict verdict, bool combined);

#endif // NTP_SELECT_H
