/*
 * How good the ESP's NTP time really is, on a clock with GNSS: every second
 * the PIC is locked on GNSS (its TIME-AUTH bound within NTP_GNSS_MAX_NS),
 * the ESP's NTP clock is read at the PIC's epoch edge (captured on GPIO27)
 * and compared with the PIC's UTC for it. NTP minus GNSS, so + means the
 * ESP's NTP time is ahead. Kept since the ESP started (or "ntp gnss reset")
 * and over the last hour (phase_stats.h). The GPIO27 capture adds a few us
 * of interrupt latency, small next to NTP's own error.
 *
 * A measure of the path asymmetry and noise NTP's bound has to allow for,
 * and of what changes to the servers or their combining do to it.
 */
#ifndef NTP_GNSS_H
#define NTP_GNSS_H

#include <Arduino.h>

#define NTP_GNSS_MAX_NS 1000U

// Every pass of the main loop, after pic_link_service().
void ntp_gnss_service(void);
void ntp_gnss_reset(void);
// "ntp gnss", and in "ntp tracking" and "ntp -v". Says so if there is
// nothing to show (no GNSS lock seen). verbose: also the last 24 hours'
// minutes (since the start, if less), each one's mean, oldest first.
void ntp_gnss_print(Stream *output, bool verbose);

#endif // NTP_GNSS_H
