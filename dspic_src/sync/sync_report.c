#include "sync_report.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "../console/print_queue.h"
#include "../console/text.h"
#include "../platform/utc.h"

static void print_time(const SyncTime *time)
{
    UtcCivil civil;

    if (!time->known)
    {
        printf("time unknown");
        return;
    }
    utc_to_civil(time->utc_seconds, time->leap_second, &civil);
    printf("%04u-%02u-%02uT%02u:%02u:%02uZ", civil.year, civil.month, civil.day, civil.hour,
           civil.minute, civil.second);
}


static void print_transition(const SyncTransition *transition)
{
    printf("epoch %lu: %s -> %s, %s (%s", (unsigned long)transition->epoch,
           sync_state_name(transition->from), sync_state_name(transition->to),
           sync_source_name(transition->source), sync_reason_name(transition->reason));
    if (transition->reason == SYNC_REASON_SOURCE_LOST)
    {
        printf(": %s", sync_source_check_name((SyncCheck)transition->verdict));
    }
    printf(")");
}


// The date and time as print_time() gives it.
static void text_time(TextLine *line, const SyncTime *time)
{
    UtcCivil civil;

    if (!time->known)
    {
        text_str(line, "time unknown");
        return;
    }
    utc_to_civil(time->utc_seconds, time->leap_second, &civil);
    text_u32(line, civil.year, 4, '0');
    text_char(line, '-');
    text_u32(line, civil.month, 2, '0');
    text_char(line, '-');
    text_u32(line, civil.day, 2, '0');
    text_char(line, 'T');
    text_u32(line, civil.hour, 2, '0');
    text_char(line, ':');
    text_u32(line, civil.minute, 2, '0');
    text_char(line, ':');
    text_u32(line, civil.second, 2, '0');
    text_char(line, 'Z');
}


// A bound in ns, us or ms, whichever keeps it short.
static void text_bound(TextLine *line, uint32_t ns)
{
    if (ns == SYNC_BOUND_NONE)
    {
        text_char(line, '-');
    }
    else if (ns < 10000UL)
    {
        text_u32(line, ns, 0, ' ');
        text_str(line, "ns");
    }
    else if (ns < 10000000UL)
    {
        text_u32(line, ns / 1000UL, 0, ' ');
        text_str(line, "us");
    }
    else
    {
        text_u32(line, ns / 1000000UL, 0, ' ');
        text_str(line, "ms");
    }
}


static const char *bound_unit(uint32_t ns, uint32_t *scaled)
{
    if (ns < 10000UL)
    {
        *scaled = ns;
        return "ns";
    }
    if (ns < 10000000UL)
    {
        *scaled = ns / 1000UL;
        return "us";
    }
    *scaled = ns / 1000000UL;
    return "ms";
}


// The candidate in use: the selected source, or holdover.
static uint8_t current_candidate(const SyncStatus *status)
{
    if (status->state == SYNC_HOLDOVER)
    {
        return SYNC_CANDIDATE_HOLDOVER;
    }
    if (status->state == SYNC_ACQUIRE || status->state == SYNC_LOCKED)
    {
        return status->source;
    }
    return SYNC_SOURCE_NONE;
}


void sync_report_transition(const SyncUpdate *update)
{
    // Rare: printf will do.
    if (update->transitioned)
    {
        printf("      SYNC *** ");
        print_transition(sync_last_transition());
        printf("\r\n");
    }
}


void sync_report_epoch(const SyncUpdate *update)
{
    const SyncStatus *status = sync_status();
    TextLine line;

    text_start(&line);
    text_str(&line, "      SYNC: ");
    text_str(&line, sync_state_name(status->state));
    text_char(&line, ' ');
    text_str(&line, sync_source_name(status->source));
    text_str(&line, status->fallback ? " (fallback) " : " ");
    text_time(&line, &status->time);
    if (status->state == SYNC_HOLDOVER)
    {
        text_str(&line, " holdover ");
        text_u32(&line, status->holdover_epochs, 0, ' ');
        text_str(&line, "s +/-");
        text_bound(&line, status->holdover.bound_ns);
    }
    if (status->source < SYNC_SOURCE_COUNT)
    {
        const SyncSourceTracker *tracker = &status->sources[status->source];
        text_char(&line, ' ');
        text_str(&line, sync_source_check_name(tracker->check));
        if (tracker->report.uncertainty_ns != 0U)
        {
            text_str(&line, " unc ");
            text_u32(&line, tracker->report.uncertainty_ns, 0, ' ');
            text_str(&line, "ns");
        }
        if (tracker->bound_ns != 0U)
        {
            text_str(&line, " +/-");
            text_bound(&line, tracker->bound_ns);
        }
    }
    else
    {
        // Nothing selected: why each source is not, and how far it has got.
        uint8_t index;
        for (index = 0; index < SYNC_SOURCE_COUNT; index++)
        {
            const SyncSourceTracker *tracker = &status->sources[index];
            text_str(&line, index == 0U ? " | " : ", ");
            text_str(&line, sync_source_name(index));
            text_char(&line, ' ');
            text_str(&line, sync_source_check_name(tracker->check));
            if (tracker->report.uncertainty_ns != 0U)
            {
                text_str(&line, " unc ");
                text_u32(&line, tracker->report.uncertainty_ns, 0, ' ');
                text_str(&line, "ns");
            }
            text_char(&line, ' ');
            text_u32(&line, tracker->good_run, 0, ' ');
            text_char(&line, '/');
            text_u32(&line, SYNC_QUALIFY_EPOCHS, 0, ' ');
        }
    }
    text_str(&line, update->usable ? "\r\n" : " (not steering)\r\n");
    text_send(&line);
}


void sync_report_sources(void)
{
    const SyncStatus *status = sync_status();
    uint8_t current = current_candidate(status);
    uint8_t pass;
    uint8_t candidate;
    TextLine line;

    text_start(&line);
    text_str(&line, "   SOURCES:");
    // Ranked first, best first (ranks 1 up), then the rest (rank 0).
    for (pass = 1; pass <= SYNC_CANDIDATE_HOLDOVER + 1U; pass++)
    {
        uint8_t rank = (uint8_t)(pass % (SYNC_CANDIDATE_HOLDOVER + 1U));
        for (candidate = 0; candidate <= SYNC_CANDIDATE_HOLDOVER; candidate++)
        {
            if (candidate == SYNC_SOURCE_NONE || sync_candidate_rank(candidate) != rank)
            {
                continue;
            }
            text_char(&line, ' ');
            if (rank == 0U)
            {
                text_char(&line, '-');
            }
            else
            {
                text_u32(&line, rank, 0, ' ');
            }
            text_char(&line, candidate == current ? '*' : candidate == status->second ? '+' : ' ');
            text_str(&line, sync_source_name(candidate));
            text_char(&line, ' ');
            if (candidate == SYNC_CANDIDATE_HOLDOVER)
            {
                text_bound(&line, sync_candidate_bound(candidate));
                if (status->holdover.ready)
                {
                    text_char(&line, ' ');
                    text_u32(&line, status->holdover.age, 0, ' ');
                    text_char(&line, 's');
                }
            }
            else
            {
                const SyncSourceTracker *tracker = &status->sources[candidate];
                text_bound(&line, tracker->bound_ns != 0U ? tracker->bound_ns : SYNC_BOUND_NONE);
                text_char(&line, ' ');
                text_str(&line, sync_source_check_name(tracker->check));
                text_char(&line, ' ');
                text_u32(&line, tracker->good_run, 0, ' ');
                if (tracker->phase_valid)
                {
                    text_str(&line, tracker->report.captured ? " cap " : " ioc ");
                    text_i32(&line, tracker->phase_ticks, 0, true);
                }
            }
            text_str(&line, " |");
        }
    }
    if (status->challenger != SYNC_SOURCE_NONE)
    {
        text_str(&line, " challenger ");
        text_str(&line, sync_source_name(status->challenger));
        text_char(&line, ' ');
        text_u32(&line, status->challenge_run, 0, ' ');
        text_char(&line, '/');
        text_u32(&line, SYNC_SWITCH_EPOCHS, 0, ' ');
    }
    text_str(&line, "\r\n");
    text_send(&line);
}


/*
 * What the report prints from: the status, and what the candidates table
 * asks of the synchroniser besides, taken at one moment. The report printed
 * in one go reads the live status; printed in parts (sync_report_queue),
 * every part reads a copy taken by its first part, so the parts agree
 * whatever epochs pass between them.
 */
#define CANDIDATES (SYNC_CANDIDATE_HOLDOVER + 1U)
typedef struct
{
    const SyncStatus *status;
    uint32_t bound[CANDIDATES];
    uint8_t rank[CANDIDATES];
    bool steers[CANDIDATES];
    bool synchronised;
} SyncView;

static SyncStatus copied; // the parts' copy, about half a KB
static SyncView parts_view;

static void take_view(SyncView *view, const SyncStatus *status)
{
    uint8_t candidate;

    view->status = status;
    for (candidate = 0; candidate < CANDIDATES; candidate++)
    {
        view->bound[candidate] = sync_candidate_bound(candidate);
        view->rank[candidate] = sync_candidate_rank(candidate);
        view->steers[candidate] = sync_source_steers(candidate);
    }
    view->synchronised = sync_is_synchronised();
}


static void print_candidates(const SyncView *taken)
{
    const SyncStatus *status = taken->status;
    uint8_t current = current_candidate(status);
    uint8_t candidate;

    printf("Candidates (bound, rank; * in use, + second choice):\r\n");
    for (candidate = 0; candidate <= SYNC_CANDIDATE_HOLDOVER; candidate++)
    {
        uint32_t bound = taken->bound[candidate];
        uint32_t scaled;
        const char *unit;

        if (candidate == SYNC_SOURCE_NONE)
        {
            continue;
        }
        printf("  %c %-5s",
               candidate == current          ? '*'
               : candidate == status->second ? '+'
                                             : ' ',
               sync_source_name(candidate));
        if (candidate < SYNC_SOURCE_COUNT && !taken->steers[candidate])
        {
            if (status->sources[candidate].bound_ns != 0U)
            {
                unit = bound_unit(status->sources[candidate].bound_ns, &scaled);
                printf(" +/-%lu%s,", (unsigned long)scaled, unit);
            }
            printf(" time of day only");
        }
        else if (bound == SYNC_BOUND_NONE && candidate < SYNC_SOURCE_COUNT &&
                 status->sources[candidate].usable_run >= SYNC_QUALIFY_EPOCHS)
        {
            unit = bound_unit(status->sources[candidate].bound_ns, &scaled);
            printf(" +/-%lu%s, fallback only (from free run)", (unsigned long)scaled, unit);
        }
        else if (bound == SYNC_BOUND_NONE)
        {
            printf(" not a candidate");
        }
        else
        {
            unit = bound_unit(bound, &scaled);
            printf(" +/-%lu%s rank %u", (unsigned long)scaled, unit,
                   (unsigned)taken->rank[candidate]);
        }
        if (candidate == SYNC_CANDIDATE_HOLDOVER)
        {
            if (status->holdover.ready)
            {
                unit = bound_unit(status->holdover.base_ns, &scaled);
                printf(", from %s %lus ago at +/-%lu%s, %luns/s",
                       sync_source_name(status->holdover.source),
                       (unsigned long)status->holdover.age, (unsigned long)scaled, unit,
                       (unsigned long)status->holdover.frequency_ns);
            }
        }
        else
        {
            const SyncSourceTracker *tracker = &status->sources[candidate];
            printf(", %s for %u", sync_source_check_name(tracker->check),
                   (unsigned)tracker->good_run);
            if (tracker->phase_valid)
            {
                printf(", %s phase %+ld ticks", tracker->report.captured ? "captured" : "watched",
                       (long)tracker->phase_ticks);
            }
        }
        printf("\r\n");
    }
    if (status->challenger != SYNC_SOURCE_NONE)
    {
        printf("  %s is challenging: %u of %u epochs\r\n", sync_source_name(status->challenger),
               (unsigned)status->challenge_run, SYNC_SWITCH_EPOCHS);
    }
}


static void print_source(uint8_t index, const SyncSourceTracker *tracker)
{
    uint8_t check;

    printf("%-5s %-12s run %u, last good epoch %lu, unc %luns, label jumps %lu\r\n",
           sync_source_name(index), sync_source_check_name(tracker->check),
           (unsigned)tracker->good_run, (unsigned long)tracker->last_good_epoch,
           (unsigned long)tracker->report.uncertainty_ns, (unsigned long)tracker->label_jumps);
    printf("      verdicts:");
    for (check = 0; check < (uint8_t)SYNC_CHECK_COUNT; check++)
    {
        if (tracker->verdicts[check] != 0U)
        {
            printf(" %s %lu", sync_source_check_name((SyncCheck)check),
                   (unsigned long)tracker->verdicts[check]);
        }
    }
    printf("\r\n");
    if (tracker->cross_checks != 0U)
    {
        printf("      cross-check: %lu, %+ld..%+ldns, latest %+ldns\r\n",
               (unsigned long)tracker->cross_checks, (long)tracker->cross_check_min_ns,
               (long)tracker->cross_check_max_ns, (long)tracker->report.cross_check_ns);
    }
}


static bool print_part(const SyncView *taken, uint8_t part, bool verbose)
{
    const SyncStatus *status = taken->status;
    uint32_t first;
    uint32_t index;

    if (part == 0U)
    {
        printf("Sync: %s", sync_state_name(status->state));
        if (status->source != SYNC_SOURCE_NONE)
        {
            printf(" on %s%s", sync_source_name(status->source),
                   status->fallback ? " (FALLBACK)" : "");
        }
        printf(", ");
        utc_print_duration(status->state_epochs);
        printf(", %s\r\n", taken->synchronised ? "SYNCHRONISED" : "NOT SYNCHRONISED");
        printf("  Time: ");
        print_time(&status->time);
        if (status->time.known)
        {
            printf(" (%s)", sync_source_name(status->time.source));
        }
        if (verbose)
        {
            printf(", %lu corrections", (unsigned long)status->time.corrections);
        }
        if (status->time.leap.change != 0)
        {
            UtcCivil civil;
            utc_to_civil(status->time.leap.event_utc - 1, false, &civil);
            printf(", leap %s %04u-%02u-%02uT%s", status->time.leap.change > 0 ? "+1" : "-1",
                   civil.year, civil.month, civil.day,
                   status->time.leap.change > 0 ? "23:59:60Z" : "23:59:59Z");
        }
        printf("\r\n");
        if (verbose && status->aligned)
        {
            printf("Epoch put on the %s second at start-up: moved ",
                   sync_source_name(status->aligned_to));
            utc_print_ns((int64_t)status->aligned_ticks * (1000 / (int64_t)TIMING_TICKS_PER_US),
                         true);
            printf("\r\n");
        }
        return verbose;
    }
    if (!verbose || part >= SYNC_REPORT_PARTS)
    {
        return false;
    }
    if (part == 1U)
    {
        print_candidates(taken);
        return true;
    }
    if (part < 2U + SYNC_SOURCE_COUNT)
    {
        print_source((uint8_t)(part - 2U), &status->sources[part - 2U]);
        return true;
    }
    printf("Transitions: %lu\r\n", (unsigned long)status->transitions);
    first =
        status->transitions > SYNC_HISTORY_LENGTH ? status->transitions - SYNC_HISTORY_LENGTH : 0U;
    for (index = first; index < status->transitions; index++)
    {
        printf("  ");
        print_transition(&status->history[index % SYNC_HISTORY_LENGTH]);
        printf("\r\n");
    }
    return false;
}


bool sync_report_part(uint8_t part, bool verbose)
{
    if (part == 0U)
    {
        copied = *sync_status();
        take_view(&parts_view, &copied);
    }
    return print_part(&parts_view, part, verbose);
}


// A part per step, each queueing the next.
static void part_step(uint32_t now_ms, uint16_t part, bool verbose)
{
    (void)now_ms;
    if (sync_report_part((uint8_t)part, verbose))
    {
        (void)print_queue_add(part_step, (uint16_t)(part + 1U), verbose);
    }
}


bool sync_report_queue(bool verbose)
{
    return print_queue_add(part_step, 0, verbose);
}


void sync_report_status(bool verbose)
{
    SyncView live;
    uint8_t part = 0;

    take_view(&live, sync_status());
    while (print_part(&live, part++, verbose))
    {
    }
}
