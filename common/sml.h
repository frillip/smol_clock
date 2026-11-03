/*
 * SML codec: framing, CRC and payload conversion for the dsPIC <-> ESP32
 * link (common/SML_PROTOCOL.md, sml_proto.h). Portable C99 with no
 * hardware or clock dependency, so the ESP32 can build the same file.
 *
 *   B5 73 CLASS ID LEN_H LEN_L PAYLOAD[LEN] CRC_H CRC_L
 *
 * Payloads are big-endian on the wire. The packed structs in sml_proto.h
 * fix the order and size of their fields; sml_encode()/sml_decode() convert
 * between such a struct (native byte order) and the wire, field by field,
 * driven by each message's layout: its field sizes in order. Because the
 * structs are packed, the wire offset of every field equals its offset in
 * the struct, so one layout string describes both.
 *
 * Receiving: feed every byte to sml_parser_push(). It returns true with a
 * complete, CRC-checked frame; bytes that are not part of a frame are
 * counted as noise and dropped (on a link still carrying text, that text).
 * After a bad length or CRC the parser rescans what it buffered, so a real
 * frame that began inside a false header is not lost.
 */
#ifndef SML_H
#define SML_H

#include <stdbool.h>
#include <stdint.h>

#include "sml_proto.h"

// CRC-16/IBM-3740 (CCITT-FALSE): poly 0x1021, init 0xFFFF, no reflection.
#define SML_CRC_INIT 0xFFFFU

// Directions, for the message table.
#define SML_FROM_PIC 0x01U
#define SML_FROM_ESP 0x02U

typedef struct
{
    uint8_t class_id;
    uint8_t message_id;
    uint8_t length;        // payload bytes; 0: variable (STAT-DEBUG)
    uint8_t direction;     // SML_FROM_*
    const uint8_t *layout; // field sizes in order, 0-terminated: 2 and 4 integers, else bytes
    const char *name;      // "TIME-AUTH", ...
} SmlMessage;

typedef struct
{
    uint8_t class_id;
    uint8_t message_id;
    uint16_t length;
    const uint8_t *payload; // valid until the next call into the parser
} SmlFrame;

typedef struct
{
    uint32_t frames;        // complete, CRC-good frames
    uint32_t noise_bytes;   // bytes outside any frame
    uint32_t crc_errors;    // a header and the full length arrived, CRC wrong
    uint32_t length_errors; // header with a length over SML_MAX_PAYLOAD_BYTES
    uint32_t timeouts;      // partial frames abandoned by sml_parser_timeout()
} SmlParserStats;

typedef struct
{
    uint8_t buffer[SML_MAX_FRAME_BYTES];
    uint16_t count;    // bytes buffered
    uint16_t consumed; // the frame last returned, released on the next call
    SmlParserStats stats;
} SmlParser;

uint16_t sml_crc(uint16_t crc, const uint8_t *data, uint16_t length);

// The table entry for a class and ID, or NULL if the message is unknown.
// Not built with SML_NO_MESSAGES (the SPL), nor is sml_frame_message().
const SmlMessage *sml_message(uint8_t class_id, uint8_t message_id);
// Every known message, for diagnostics and tests.
const SmlMessage *sml_messages(uint8_t *count);

// Native packed struct -> wire, and back. layout from the message table.
// Return the number of payload bytes converted.
uint16_t sml_encode(const uint8_t *layout, const void *native, uint8_t *wire);
uint16_t sml_decode(const uint8_t *layout, const uint8_t *wire, void *native);

// Big-endian field access, for code that builds or reads payloads directly.
void sml_put_u16(uint8_t *wire, uint16_t value);
void sml_put_u32(uint8_t *wire, uint32_t value);
uint16_t sml_get_u16(const uint8_t *wire);
uint32_t sml_get_u32(const uint8_t *wire);

/* A whole frame around a payload already in wire order. frame must hold
 * SML_HEADER_BYTES + length + SML_CRC_BYTES; payload may already sit at
 * frame + SML_HEADER_BYTES. Returns the frame length, or 0 if length is
 * over SML_MAX_PAYLOAD_BYTES. */
uint16_t sml_frame(uint8_t class_id, uint8_t message_id, const uint8_t *payload, uint16_t length,
                   uint8_t *frame);
// The same for a fixed-length message from its native struct (encoded per
// the table). frame: SML_MAX_FRAME_BYTES. Returns 0 if the message is
// unknown or variable-length.
uint16_t sml_frame_message(uint8_t class_id, uint8_t message_id, const void *native,
                           uint8_t *frame);

void sml_parser_init(SmlParser *parser);
// One received byte. True if it completed a frame, returned in *frame.
bool sml_parser_push(SmlParser *parser, uint8_t byte, SmlFrame *frame);
// Another complete frame already buffered (only after a rescan, rare).
// Call after a push returned true, until it returns false.
bool sml_parser_next(SmlParser *parser, SmlFrame *frame);
// The link went quiet with a frame part-received: drop what is buffered.
// True if anything was dropped.
bool sml_parser_timeout(SmlParser *parser);

#endif // SML_H
