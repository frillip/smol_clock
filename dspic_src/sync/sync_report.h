// Synchroniser diagnostics on stdout (the UART1 console).
#ifndef SYNC_REPORT_H
#define SYNC_REPORT_H

#include "sync.h"

// A line for a state change, if there was one this epoch. Printed whatever
// the debug settings: changes are rare and always worth seeing.
void sync_report_transition(const SyncUpdate *update);

// One line per epoch: state, source and the UTC second of our epoch.
void sync_report_epoch(const SyncUpdate *update);

// One line per epoch: every candidate, best first, with its bound, verdict
// and phase; * marks the one in use, + the second choice.
void sync_report_sources(void);

// The state and what it steers by, and the time of day. verbose: the
// candidates ranked, each source's record and the recent state changes.
void sync_report_status(bool verbose);

// The same in parts, for printing a piece at a time (console/print_queue.h):
// 0 the state and the time, 1 the candidates, then each source, then the
// state changes; non-verbose, part 0 alone. True while more parts follow.
// Part 0 copies the synchroniser's state (about half a KB) and every part
// prints from that copy, so the parts agree however many epochs pass
// between them; parts are printed in order, one report at a time.
#define SYNC_REPORT_PARTS (3U + SYNC_SOURCE_COUNT)
bool sync_report_part(uint8_t part, bool verbose);
// The parts queued, each queueing the next: one place in the queue. False
// if full.
bool sync_report_queue(bool verbose);

#endif // SYNC_REPORT_H
