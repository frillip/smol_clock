#include "ntp_select.h"

#include <math.h>
#include <string.h>

// What the followed server is chosen by: rank_us if given, else the distance.
static int64_t rank_us(const NtpCandidate *candidate)
{
    return candidate->rank_us > 0 ? candidate->rank_us : candidate->distance_us;
}


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
            if (selection->selected < 0 || rank_us(c) < rank_us(&candidates[selection->selected]))
            {
                selection->selected = index;
            }
        }
    }
    // Keep the server in use unless another is twice as close.
    if (current >= 0 && current < count && (best & (1U << current)) != 0 &&
        rank_us(&candidates[selection->selected]) * 2 >= rank_us(&candidates[current]))
    {
        selection->selected = current;
    }
    if (selection->selected >= 0)
    {
        selection->verdict[selection->selected] = NTP_VERDICT_SELECTED;
    }
}


void ntp_combine(const NtpCandidate *candidates, int count, uint8_t *distant,
                 NtpSelection *selection)
{
    const NtpCandidate *followed;
    double weights = 0.0;
    double sum = 0.0;
    double frequency_weights = 0.0;
    double frequency_sum = 0.0;
    double inverse_skews = 0.0;
    int64_t best_us;
    int index;

    selection->combined = 0;
    selection->frequency_known = false;
    selection->combined_frequency_ppb = 0.0;
    selection->combined_skew_ppb = 0.0;
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
    // The limit runs from the least distance among those that could be
    // combined, not the followed server's: the followed one is kept until
    // another's distance is under half of its, so it may be the furthest,
    // and a limit from it let every server in (2026-11-11). chronyd follows
    // the least distance, so its limit from the selected one is the same.
    best_us = followed->distance_us;
    for (index = 0; index < count && index < NTP_SELECT_MAX; index++)
    {
        const NtpCandidate *c = &candidates[index];

        if (selection->majority && selection->verdict[index] == NTP_VERDICT_CANDIDATE && !c->young &&
            c->distance_us < best_us)
        {
            best_us = c->distance_us;
        }
    }
    for (index = 0; index < count && index < NTP_SELECT_MAX; index++)
    {
        const NtpCandidate *c = &candidates[index];
        double weight;

        if (index != selection->selected)
        {
            if (!selection->majority || selection->verdict[index] != NTP_VERDICT_CANDIDATE ||
                c->young)
            {
                continue; // young: not combined yet, and no distant count either
            }
            if (c->distance_us > NTP_COMBINE_LIMIT * (double)(NTP_COMBINE_MARGIN_US + best_us) ||
                (c->fitted && followed->fitted &&
                 fabs(c->frequency_ppb - followed->frequency_ppb) >
                     NTP_COMBINE_LIMIT * (c->skew_ppb + followed->skew_ppb)))
            {
                if (distant[index] < NTP_COMBINE_DISTANT)
                {
                    distant[index] = (uint8_t)(distant[index] + NTP_COMBINE_DISTANT_STEP);
                }
                continue;
            }
            if (distant[index] != 0)
            {
                distant[index]--;
                continue;
            }
        }
        weight = 1.0 / (double)(c->distance_us > 0 ? c->distance_us : 1);
        weights += weight;
        sum += weight * (double)(c->offset_us - followed->offset_us);
        if (c->fitted && c->frequency_sd_ppb > 0.0 && c->skew_ppb > 0.0)
        {
            double frequency_weight = 1.0 / (c->frequency_sd_ppb * c->frequency_sd_ppb);

            frequency_weights += frequency_weight;
            frequency_sum += frequency_weight * c->frequency_ppb;
            inverse_skews += 1.0 / (c->skew_ppb * c->skew_ppb);
        }
        selection->in_combined[index] = true;
        selection->combined++;
    }
    // Relative to the followed server's offset, so the sum stays small.
    selection->combined_offset_us = followed->offset_us + (int64_t)llround(sum / weights);
    if (frequency_weights > 0.0)
    {
        selection->frequency_known = true;
        selection->combined_frequency_ppb = frequency_sum / frequency_weights;
        selection->combined_skew_ppb = 1.0 / sqrt(inverse_skews);
    }
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
