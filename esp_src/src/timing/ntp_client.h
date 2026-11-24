/*
 * NTP client: one poll is a burst of NTP_CLIENT_BURST exchanges with the
 * server, NTP_CLIENT_SPACING_MS apart, run in its own task so the network
 * never blocks the main loop. The exchange with the shortest round trip is
 * the poll's sample (the least queueing, so the least asymmetry).
 *
 * The server's name is resolved by the caller, in the loop task, not here.
 * The Arduino core's Network.hostByName() (used by HTTPClient) calls lwIP's
 * dns_clear_cache() without the TCPIP core lock the first time after the
 * addresses change (so at least once after every start). dns_clear_cache()
 * aborts any query still pending, removing its UDP pcb, and lwIP asserts
 * in udp_remove(). A lookup pending in this task when the loop starts its
 * first HTTPS request crashed the ESP. With every DNS lookup in the loop
 * task, none is ever pending when that happens.
 *
 * The server may be IPv4 or IPv6: ntp.cpp resolves both (net_connect.h),
 * polls the preferred family's address, and the other's if that one never
 * answers.
 *
 * T1 and T4 are esp_timer microseconds, as close to the wire as lwIP lets
 * them be (2026-11-11, was around sendto and recvfrom): T1 once udp_sendto
 * has handed the request to the Wi-Fi driver, T4 in lwIP's own task as the
 * reply arrives, before the client task is woken. The requests carry DSCP
 * EF, so Wi-Fi with WMM queues them ahead of bulk traffic.
 */
#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_clock.h"
#include "ntp_packet.h"

#define NTP_CLIENT_BURST      4
#define NTP_CLIENT_SPACING_MS 2000
#define NTP_CLIENT_TIMEOUT_MS 1000
#define NTP_CLIENT_DSCP       46 // EF, expedited forwarding (TOS 0xB8)

typedef struct
{
    bool ok;               // at least one good exchange
    EspClockSample sample; // the best one
    uint8_t replies;       // good exchanges in the burst
    bool dns_failed;       // set by the caller (ntp.cpp)
    NtpReplyResult last_result;
    // The server's identity (ntp_packet.h) at the burst's first good reply
    // and its last: two servers are compared at whichever were nearest.
    NtpIdentity first;
    NtpIdentity last;
    char kiss[5];     // kiss code, if the server sent one
    char address[48]; // the server's address as used: 192.0.2.10, 2001:db8::123
} NtpPollResult;

// A server's address: IPv4 (the first 4 bytes, network order) or IPv6 (16
// bytes, and the zone of a link-local one).
typedef struct
{
    bool ipv6;
    uint8_t bytes[16];
    uint8_t zone;
} NtpServerAddress;

typedef struct
{
    uint32_t polls;
    uint32_t good_polls;
    uint32_t exchanges;
    uint32_t timeouts;
    uint32_t rejected[NTP_REPLY_COUNT];
} NtpClientStats;

void ntp_client_init(void);
// Starts a poll of the server at address unless one is running. False if
// one is running.
bool ntp_client_poll(const NtpServerAddress *address);
bool ntp_client_busy(void);
// The result of a finished poll, once.
bool ntp_client_take(NtpPollResult *result);
const NtpClientStats *ntp_client_stats(void);

#endif // NTP_CLIENT_H
