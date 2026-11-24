#include "ntp_packet.h"

#include <string.h>

#define NTP_UNIX_OFFSET_S 2208988800LL // 1900-01-01 to 1970-01-01
#define NTP_ERA_S         4294967296LL

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}


static uint64_t get_u64(const uint8_t *data)
{
    return (uint64_t)get_u32(data) << 32 | get_u32(data + 4);
}


// NTP short format (16.16 seconds) to microseconds.
static uint32_t short_to_us(uint32_t value)
{
    return (uint32_t)(((uint64_t)value * 1000000ULL) >> 16);
}


int64_t ntp_timestamp_to_unix_us(uint64_t timestamp)
{
    int64_t seconds = (int64_t)(timestamp >> 32);
    int64_t micros = (int64_t)(((timestamp & 0xFFFFFFFFULL) * 1000000ULL) >> 32);

    if (seconds < 0x80000000LL)
    {
        seconds += NTP_ERA_S;
    }
    return (seconds - NTP_UNIX_OFFSET_S) * 1000000LL + micros;
}


void ntp_packet_request(uint8_t *packet, uint64_t cookie)
{
    int index;

    memset(packet, 0, NTP_PACKET_BYTES);
    packet[0] = (0U << 6) | (4U << 3) | 3U; // no leap warning, version 4, client
    for (index = 0; index < 8; index++)
    {
        packet[40 + index] = (uint8_t)(cookie >> (56 - 8 * index));
    }
}


NtpReplyResult ntp_packet_parse(const uint8_t *packet, uint32_t length, uint64_t cookie,
                                NtpReply *reply)
{
    uint64_t receive;
    uint64_t transmit;

    memset(reply, 0, sizeof(*reply));
    if (length < NTP_PACKET_BYTES)
    {
        return NTP_REPLY_SHORT;
    }
    if ((packet[0] & 0x07U) != 4U)
    {
        return NTP_REPLY_NOT_SERVER;
    }
    if (get_u64(packet + 24) != cookie)
    {
        return NTP_REPLY_WRONG_ORIGIN;
    }
    reply->leap = packet[0] >> 6;
    reply->stratum = packet[1];
    if (reply->stratum == 0)
    {
        memcpy(reply->kiss, packet + 12, 4);
        reply->kiss[4] = '\0';
        return NTP_REPLY_KISS;
    }
    if (reply->leap == 3U || reply->stratum > 15U)
    {
        return NTP_REPLY_UNSYNCHRONISED;
    }
    reply->identity.stratum = reply->stratum;
    reply->identity.root_delay = get_u32(packet + 4);
    reply->identity.refid = get_u32(packet + 12);
    reply->identity.reference = get_u64(packet + 16);
    reply->root_delay_us = short_to_us(get_u32(packet + 4));
    reply->root_dispersion_us = short_to_us(get_u32(packet + 8));
    receive = get_u64(packet + 32);
    transmit = get_u64(packet + 40);
    if (receive == 0 || transmit == 0)
    {
        return NTP_REPLY_BAD_TIME;
    }
    reply->receive_utc_us = ntp_timestamp_to_unix_us(receive);
    reply->transmit_utc_us = ntp_timestamp_to_unix_us(transmit);
    if (reply->transmit_utc_us < reply->receive_utc_us)
    {
        return NTP_REPLY_BAD_TIME;
    }
    // A reference time of whole seconds (some appliances truncate it, and
    // update it every second) could be the same from two hosts: not used.
    reply->identity.valid = (reply->identity.reference & 0xFFFFFFFFULL) != 0;
    return NTP_REPLY_OK;
}


bool ntp_identity_same(const NtpIdentity *a, const NtpIdentity *b)
{
    return a->valid && b->valid && a->stratum == b->stratum && a->refid == b->refid &&
           a->root_delay == b->root_delay && a->reference == b->reference;
}


const char *ntp_reply_name(NtpReplyResult result)
{
    static const char *const names[NTP_REPLY_COUNT] = {"ok",
                                                       "short",
                                                       "not a server reply",
                                                       "wrong origin",
                                                       "kiss-o'-death",
                                                       "unsynchronised",
                                                       "bad timestamps"};

    return result < NTP_REPLY_COUNT ? names[result] : "?";
}
