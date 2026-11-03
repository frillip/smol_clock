#include "manifest.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool numeric_version(const char *version)
{
    bool expecting_digit = true;
    const char *p;

    if (version == NULL || *version == '\0')
    {
        return false;
    }
    for (p = version; *p != '\0'; p++)
    {
        if (*p == '.')
        {
            if (expecting_digit)
            {
                return false;
            }
            expecting_digit = true;
        }
        else if (isdigit((unsigned char)*p))
        {
            expecting_digit = false;
        }
        else
        {
            return false;
        }
    }
    return !expecting_digit;
}


bool manifest_compare_versions(const char *a, const char *b, int *result)
{
    if (!numeric_version(a) || !numeric_version(b))
    {
        return false;
    }
    while (*a != '\0' || *b != '\0')
    {
        const char *a_end = a + strcspn(a, ".");
        const char *b_end = b + strcspn(b, ".");
        size_t a_length;
        size_t b_length;
        int order;

        while (a < a_end && *a == '0')
        {
            a++;
        }
        while (b < b_end && *b == '0')
        {
            b++;
        }
        a_length = (size_t)(a_end - a);
        b_length = (size_t)(b_end - b);
        if (a_length != b_length)
        {
            *result = a_length > b_length ? 1 : -1;
            return true;
        }
        order = strncmp(a, b, a_length);
        if (order != 0)
        {
            *result = order > 0 ? 1 : -1;
            return true;
        }
        a = *a_end == '.' ? a_end + 1 : a_end;
        b = *b_end == '.' ? b_end + 1 : b_end;
    }
    *result = 0;
    return true;
}


// A component, checked, into out (when out is given). False if malformed.
static bool component(JsonVariantConst value, ManifestComponent *out, const char *name, char *why,
                      size_t why_size)
{
    const char *version;
    const char *url;
    uint8_t digest[SHA256_BYTES];

    if (value.isNull())
    {
        if (out != NULL)
        {
            memset(out, 0, sizeof(*out));
        }
        return true;
    }
    version = value["version"].as<const char *>();
    url = value["url"].as<const char *>();
    if (!value.is<JsonObjectConst>() || !value["version"].is<const char *>() ||
        !numeric_version(version) || strlen(version) >= sizeof(out->version))
    {
        snprintf(why, why_size, "%s: no version", name);
        return false;
    }
    if (!value["url"].is<const char *>() || strlen(url) >= MANIFEST_URL_BYTES ||
        (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0))
    {
        snprintf(why, why_size, "%s: no URL", name);
        return false;
    }
    if (!value["size"].is<uint32_t>() || value["size"].as<uint32_t>() == 0U)
    {
        snprintf(why, why_size, "%s: no size", name);
        return false;
    }
    if (!value["sha256"].is<const char *>() ||
        !sha256_from_hex(value["sha256"].as<const char *>(), digest))
    {
        snprintf(why, why_size, "%s: no SHA-256", name);
        return false;
    }
    if (out != NULL)
    {
        out->present = true;
        snprintf(out->version, sizeof(out->version), "%s", version);
        snprintf(out->url, sizeof(out->url), "%s", url);
        out->size = value["size"].as<uint32_t>();
        memcpy(out->sha256, digest, sizeof(digest));
    }
    return true;
}


static bool optional_string(JsonVariantConst value)
{
    return value.isNull() || value.is<const char *>();
}


// One configuration, checked.
static bool configuration(JsonVariantConst entry, unsigned index, char *why, size_t why_size)
{
    char where[48];

    snprintf(where, sizeof(where), "configuration %u", index + 1U);
    if (!entry.is<JsonObjectConst>() || !entry["hardware"].is<const char *>())
    {
        snprintf(why, why_size, "%s: no hardware", where);
        return false;
    }
    if (!optional_string(entry["config"]))
    {
        snprintf(why, why_size, "%s: config not a string", where);
        return false;
    }
    if (!entry["devices"].isNull())
    {
        if (!entry["devices"].is<JsonArrayConst>())
        {
            snprintf(why, why_size, "%s: devices not a list", where);
            return false;
        }
        for (JsonVariantConst device : entry["devices"].as<JsonArrayConst>())
        {
            if (!device.is<const char *>())
            {
                snprintf(why, why_size, "%s: a device not a string", where);
                return false;
            }
        }
    }
    if (entry["esp"].isNull() && entry["pic"].isNull() && entry["pic_spl"].isNull())
    {
        snprintf(why, why_size, "%s: nothing in it", where);
        return false;
    }
    snprintf(where, sizeof(where), "configuration %u esp", index + 1U);
    if (!component(entry["esp"], NULL, where, why, why_size))
    {
        return false;
    }
    snprintf(where, sizeof(where), "configuration %u pic", index + 1U);
    if (!component(entry["pic"], NULL, where, why, why_size))
    {
        return false;
    }
    snprintf(where, sizeof(where), "configuration %u pic_spl", index + 1U);
    return component(entry["pic_spl"], NULL, where, why, why_size);
}


static bool matches(JsonVariantConst entry, const char *hardware, const char *config,
                    const char *mac)
{
    const char *entry_config = entry["config"].isNull() ? "" : entry["config"].as<const char *>();

    if (strcmp(entry["hardware"].as<const char *>(), hardware) != 0 ||
        strcmp(entry_config, config) != 0)
    {
        return false;
    }
    if (entry["devices"].isNull())
    {
        return true;
    }
    for (JsonVariantConst device : entry["devices"].as<JsonArrayConst>())
    {
        if (strcasecmp(device.as<const char *>(), mac) == 0)
        {
            return true;
        }
    }
    return false;
}


ManifestResult manifest_select(JsonDocument &doc, const char *hardware, const char *config,
                               const char *mac, ManifestEntry *entry, char *why, size_t why_size)
{
    JsonArrayConst configurations;
    unsigned index = 0;

    memset(entry, 0, sizeof(*entry));
    why[0] = '\0';
    if (!doc.is<JsonObjectConst>() || !doc["format"].is<int>())
    {
        snprintf(why, why_size, "no format");
        return MANIFEST_INVALID;
    }
    if (doc["format"].as<int>() != MANIFEST_FORMAT)
    {
        snprintf(why, why_size, "format %d, not %d", doc["format"].as<int>(), MANIFEST_FORMAT);
        return MANIFEST_INVALID;
    }
    if (!doc["configurations"].is<JsonArrayConst>())
    {
        snprintf(why, why_size, "no configurations");
        return MANIFEST_INVALID;
    }
    configurations = doc["configurations"].as<JsonArrayConst>();
    for (JsonVariantConst item : configurations)
    {
        if (!configuration(item, index++, why, why_size))
        {
            return MANIFEST_INVALID;
        }
    }
    for (JsonVariantConst item : configurations)
    {
        if (matches(item, hardware, config, mac))
        {
            (void)component(item["esp"], &entry->esp, "esp", why, why_size);
            (void)component(item["pic"], &entry->pic, "pic", why, why_size);
            (void)component(item["pic_spl"], &entry->pic_spl, "pic_spl", why, why_size);
            return MANIFEST_OK;
        }
    }
    return MANIFEST_NO_ENTRY;
}
