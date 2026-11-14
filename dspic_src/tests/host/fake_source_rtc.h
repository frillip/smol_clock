#ifndef FAKE_SOURCE_RTC_H
#define FAKE_SOURCE_RTC_H

#include "../../sync/source_rtc.h"

extern SourceRtcState fake_rtc_state; // kept_changes, rtc_changes
extern SourceRtcKept fake_rtc_kept;
extern SourceRtcKept fake_rtc_restored;
extern int fake_rtc_restores;

#endif // FAKE_SOURCE_RTC_H
