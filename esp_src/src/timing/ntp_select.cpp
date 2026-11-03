#include "ntp_select.h"

#include <math.h>
#include <string.h>

static int members(unsigned set)
{
    int count = 0;

    for (; set != 0; set &= set - 1)
    {
        count++;
    }
    return count;
}


static int lowest(unsigned set)
{
    int index = 0;

    while (set != 0 && (set & 1U) == 0)
    {
        set >>= 1;
        index++;
    }
    return index;
}


// The usable servers whose intervals hold point.
static unsigned holding(const NtpCandidate *candidates, int count, int64_t point)
{
    unsigned set = 0;
    int index;

    for (index = 0; index < count; index++)
    {
        const NtpCandidate *c = &candidates[index];

        if (c->usable && c->offset_us - c->distance_us <= point &&
            point <= c->offset_us + c->distance_us)
        {
            set |= 1U << index;
        }
    }
    return set;
}


// set is better than best: more servers, then the one in use, then the
// lowest-numbered server.
static bool better(unsigned set, unsigned best, int current)
{
    unsigned in_use = current >= 0 ? 1U << current : 0U;

    if (members(set) != members(best))
    {
        return members(set) > members(best);
    }
    if ((set & in_use) != (best & in_use))
    {
        return (set & in_use) != 0;
    }
    return lowest(set) < lowest(best);
}


void ntp_select(const NtpCandidate *candidates, int count, int current, NtpSelection *selection)
{
    unsigned best = 0;
    int index;

    memset(selection, 0, sizeof(*selection));
    selection->selected = -1;
    if (count > NTP_SELECT_MAX)
    {
        count = NTP_SELECT_MAX;
    }
    // Where intervals overlap most, an overlap starts or ends at one of
    // their ends: those are the only points worth trying.
    for (index = 0; index < count; index++)
    {
        const NtpCandidate *c = &candidates[index];
        unsigned set;

        if (!c->usable)
        {
            continue;
        }
        selection->usable++;
        set = holding(candidates, count, c->offset_us - c->distance_us);
        if (better(set, best, current))
        {
            best = set;
        }
        set = holding(candidates, count, c->offset_us + c->distance_us);
        if (better(set, best, current))
        {
            best = set;
        }
    }
    selection->agreeing = members(best);
    selection->majority = selection->agreeing * 2 > selection->usable;

    for (index = 0; index < count; index++)
    {
        const NtpCandidate *c = &candidates[index];

        if (!c->configured)
        {
            selection->verdict[index] = NTP_VERDICT_NONE;
        }
        else if (!c->usable)
        {
            selection->verdict[index] = NTP_VERDICT_UNUSABLE;
        }
        else if ((best & (1U << index)) == 0)
        {
            selection->verdict[index] = NTP_VERDICT_FALSETICKER;
        }
        else
        {
            selection->verdict[index] = NTP_VERDICT_CANDIDATE;
            if (selection->selected < 0 ||
                c->distance_us < candidates[selection->selected].distance_us)
            {
                selection->selected = index;
            }
        }
    }
    // Keep the server in use unless another is twice as close.
    if (current >= 0 && current < count && (best & (1U << current)) != 0 &&
        candidates[selection->selected].distance_us * 2 >= candidates[current].distance_us)
    {
        selection->selected = current;
    }
    if (selection->selected >= 0)
    {
        selection->verdict[selection->selected] = NTP_VERDICT_SELECTED;
    }
}


void ntp_combine(const NtpCandidate *candidates, int count, NtpSelection *selection)
{
    const NtpCandidate *followed;
    double weights = 0.0;
    double sum = 0.0;
    int index;

    selection->combined = 0;
    for (index = 0; index < NTP_SELECT_MAX; index++)
    {
        selection->in_combined[index] = false;
    }
    if (selection->selected < 0 || selection->selected >= count)
    {
        selection->combined_offset_us = 0;
        return;
    }
    followed = &candidates[selection->selected];
    for (index = 0; index < count && index < NTP_SELECT_MAX; index++)
    {
        const NtpCandidate *c = &candidates[index];
        double weight;

        if (index != selection->selected &&
            (!selection->majority || selection->verdict[index] != NTP_VERDICT_CANDIDATE ||
             c->distance_us > NTP_COMBINE_LIMIT * followed->distance_us))
        {
            continue;
        }
        weight = 1.0 / (double)(c->distance_us > 0 ? c->distance_us : 1);
        weights += weight;
        sum += weight * (double)(c->offset_us - followed->offset_us);
        selection->in_combined[index] = true;
        selection->combined++;
    }
    // Relative to the followed server's offset, so the sum stays small.
    selection->combined_offset_us = followed->offset_us + (int64_t)llround(sum / weights);
}


char ntp_verdict_marker(NtpVerdict verdict, bool combined)
{
    switch (verdict)
    {
        case NTP_VERDICT_UNUSABLE: return '?';
        case NTP_VERDICT_FALSETICKER: return 'x';
        case NTP_VERDICT_CANDIDATE: return combined ? '+' : '-';
        case NTP_VERDICT_SELECTED: return '*';
        default: return ' ';
    }
}
