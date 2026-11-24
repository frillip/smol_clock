/*
 * NTPv4 client packets (RFC 5905), portable for the host tests.
 *
 * The request carries a random transmit timestamp rather than our time: the
 * server echoes it as the origin timestamp, which ties the reply to the
 * request. Our own send and receive times (T1, T4) are local microseconds
 * taken around the exchange; the server's (T2, T3) are UTC.
 */
#ifndef NTP_PACKET_H
#define NTP_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#define NTP_PACKET_BYTES 48U
#define NTP_PORT         123U

typedef enum
{
    NTP_REPLY_OK = 0,
    NTP_REPLY_SHORT,          // under 48 bytes
    NTP_REPLY_NOT_SERVER,     // mode is not 4 (server)
    NTP_REPLY_WRONG_ORIGIN,   // not an answer to our request
    NTP_REPLY_KISS,           // stratum 0: kiss-o'-death (rate limit, deny)
    NTP_REPLY_UNSYNCHRONISED, // leap indicator 3, or stratum over 15
    NTP_REPLY_BAD_TIME,       // zero or reversed server timestamps
    NTP_REPLY_COUNT
} NtpReplyResult;

/*
 * What a reply says of the server itself, rather than of the time: one
 * host gives the same whichever of its addresses answered, so two servers
 * set by different addresses (IPv4 and IPv6, two names) can be found to be
 * one (ntp.cpp). The reference timestamp is the server's own last clock
 * update, to the 2^-32 s: it changes at each, so it can only be the same
 * from one host, read between two of its updates. The refid alone is not
 * enough (every GNSS stratum 1 says GPS or PPS), nor the stratum.
 */
typedef struct
{
    bool valid; // a good reply, with a reference timestamp that can be told apart
    uint8_t stratum;
    uint32_t refid;      // as sent, the 4 bytes big-endian
    uint32_t root_delay; // as sent, NTP short format
    uint64_t reference;  // as sent, NTP timestamp
} NtpIdentity;

typedef struct
{
    uint8_t leap; // leap indicator
    uint8_t stratum;
    NtpIdentity identity; // valid with NTP_REPLY_OK
    uint32_t root_delay_us;
    uint32_t root_dispersion_us;
    int64_t receive_utc_us;  // T2
    int64_t transmit_utc_us; // T3
    char kiss[5];            // the kiss code when stratum is 0
} NtpReply;

void ntp_packet_request(uint8_t *packet, uint64_t cookie);
NtpReplyResult ntp_packet_parse(const uint8_t *packet, uint32_t length, uint64_t cookie,
                                NtpReply *reply);
const char *ntp_reply_name(NtpReplyResult result);
// Both valid and the same in every part: from one host.
bool ntp_identity_same(const NtpIdentity *a, const NtpIdentity *b);

// NTP 64-bit timestamp to Unix microseconds. Era 0 for seconds from
// 0x80000000 (1968-2036), era 1 below that (2036-2104).
int64_t ntp_timestamp_to_unix_us(uint64_t timestamp);

#endif // NTP_PACKET_H
