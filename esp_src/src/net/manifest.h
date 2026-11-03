/*
 * The update manifest, format 2 (README, Updates):
 *
 *   {"format": 2, "configurations": [
 *     {"config": "development", "hardware": "v2", "devices": ["EC:64:..."],
 *      "esp":     {"version": "0.4.0", "url": "https://...", "size": n, "sha256": "..."},
 *      "pic":     {...}, "pic_spl": {...}}, ...]}
 *
 * A configuration is a set of versions tested together. The ESP takes the
 * first whose hardware is its own, whose config is its own (absent: the
 * default, production) and whose devices, if given, include its MAC address.
 * Every configuration is checked before any is used: one malformed entry
 * makes the whole file invalid.
 */
#ifndef MANIFEST_H
#define MANIFEST_H

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/sha256.h"

#define MANIFEST_FORMAT      2
#define MANIFEST_URL_BYTES   192
#define MANIFEST_MAX_BYTES   (16 * 1024)

typedef struct
{
    bool present;
    char version[16]; // "0.4.0"
    char url[MANIFEST_URL_BYTES];
    uint32_t size;
    uint8_t sha256[SHA256_BYTES];
} ManifestComponent;

typedef struct
{
    ManifestComponent esp;
    ManifestComponent pic;
    ManifestComponent pic_spl;
} ManifestEntry;

typedef enum
{
    MANIFEST_OK = 0,
    MANIFEST_INVALID,  // see why
    MANIFEST_NO_ENTRY, // valid, but no configuration for this clock
} ManifestResult;

// The entry for this clock. config "" is the default; mac as
// WiFi.macAddress() gives it (compared ignoring case). why: what is wrong,
// for MANIFEST_INVALID.
ManifestResult manifest_select(JsonDocument &doc, const char *hardware, const char *config,
                               const char *mac, ManifestEntry *entry, char *why, size_t why_size);

// Dot-separated numeric versions: a against b as 1, 0 or -1. False unless
// both are versions.
bool manifest_compare_versions(const char *a, const char *b, int *result);

#endif // MANIFEST_H
