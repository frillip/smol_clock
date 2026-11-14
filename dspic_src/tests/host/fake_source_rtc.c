/*
 * A stand-in for the RTC source (sync/source_rtc.c), for the settings: what
 * it would keep (fake_rtc_kept, counted by fake_rtc_state), and what the
 * settings put back at start-up.
 */
#include "fake_source_rtc.h"

#include <string.h>

SourceRtcState fake_rtc_state;
SourceRtcKept fake_rtc_kept;
SourceRtcKept fake_rtc_restored;
int fake_rtc_restores;

void source_rtc_kept(SourceRtcKept *kept)
{
    *kept = fake_rtc_kept;
}


void source_rtc_restore(const SourceRtcKept *kept)
{
    fake_rtc_restored = *kept;
    fake_rtc_restores++;
}


const SourceRtcState *source_rtc_state(void)
{
    return &fake_rtc_state;
}
