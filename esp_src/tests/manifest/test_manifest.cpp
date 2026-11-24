/*
 * The update manifest, format 2 (src/net/manifest.cpp): the configuration
 * this clock takes, by hardware, config and device, and every malformed
 * file refused whole.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/net/manifest.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

#define SHA "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define MAC "EC:64:C9:06:D3:50"

static const char manifest[] = R"({
  "format": 2,
  "configurations": [
    {"config": "development", "hardware": "v2", "devices": ["ec:64:c9:06:d3:50"],
     "esp": {"version": "0.4.1", "url": "https://rubidium.darksky.io/dev/esp.bin", "size": 1252320, "sha256": ")" SHA R"("},
     "pic": {"version": "0.4.0", "url": "https://rubidium.darksky.io/dev/pic.smolpic", "size": 176578, "sha256": ")" SHA R"("},
     "pic_spl": {"version": "0.1.0", "url": "https://rubidium.darksky.io/dev/spl.smolspl", "size": 7900, "sha256": ")" SHA R"("}},
    {"config": "development", "hardware": "v2",
     "esp": {"version": "0.4.0", "url": "https://rubidium.darksky.io/dev/esp.bin", "size": 1, "sha256": ")" SHA R"("}},
    {"hardware": "v3",
     "esp": {"version": "1.0.0", "url": "https://rubidium.darksky.io/v3/esp.bin", "size": 1, "sha256": ")" SHA R"("}},
    {"hardware": "v2",
     "esp": {"version": "0.3.9", "url": "http://rubidium.darksky.io/prod/esp.bin", "size": 2, "sha256": ")" SHA R"("},
     "pic": {"version": "0.3.1", "url": "https://rubidium.darksky.io/prod/pic.smolpic", "size": 3, "sha256": ")" SHA R"("}}
  ]
})";

static ManifestResult select(const char *text, const char *hardware, const char *config,
                             const char *mac, ManifestEntry *entry, char *why)
{
    JsonDocument doc;

    if (deserializeJson(doc, text))
    {
        return (ManifestResult)99;
    }
    return manifest_select(doc, hardware, config, mac, entry, why, 96);
}


// The manifest with one string replaced, for the malformed cases.
static ManifestResult with(const char *from, const char *to, char *why)
{
    std::string text = manifest;
    size_t at = text.find(from);
    ManifestEntry entry;

    if (at == std::string::npos)
    {
        printf("  (no '%s')\n", from);
        return (ManifestResult)98;
    }
    text.replace(at, strlen(from), to);
    return select(text.c_str(), "v2", "", MAC, &entry, why);
}


int main(void)
{
    ManifestEntry entry;
    char why[96];
    int order;

    printf("choosing the configuration\n");
    CHECK(select(manifest, "v2", "development", MAC, &entry, why) == MANIFEST_OK);
    CHECK(entry.esp.present && strcmp(entry.esp.version, "0.4.1") == 0);
    CHECK(entry.esp.size == 1252320 && entry.esp.sha256[0] == 0xBA && entry.esp.sha256[31] == 0xAD);
    CHECK(entry.pic.present && strcmp(entry.pic.url, "https://rubidium.darksky.io/dev/pic.smolpic") == 0);
    CHECK(entry.pic_spl.present && entry.pic_spl.size == 7900);
    CHECK(select(manifest, "v2", "development", "EC:64:C9:06:D3:51", &entry, why) == MANIFEST_OK);
    CHECK(strcmp(entry.esp.version, "0.4.0") == 0 && !entry.pic.present && !entry.pic_spl.present);
    CHECK(select(manifest, "v2", "", MAC, &entry, why) == MANIFEST_OK);
    CHECK(strcmp(entry.esp.version, "0.3.9") == 0 && strcmp(entry.pic.version, "0.3.1") == 0);
    CHECK(strncmp(entry.esp.url, "http://", 7) == 0);
    CHECK(select(manifest, "v3", "", MAC, &entry, why) == MANIFEST_OK &&
          strcmp(entry.esp.version, "1.0.0") == 0);
    CHECK(select(manifest, "v3", "development", MAC, &entry, why) == MANIFEST_NO_ENTRY);
    CHECK(select(manifest, "v4", "", MAC, &entry, why) == MANIFEST_NO_ENTRY);

    printf("issued times (make_manifest.py's): not read, accepted\n");
    CHECK(with("\"format\": 2,", "\"format\": 2, \"issued\": \"2026-11-23T13:51:07Z\",", why) ==
          MANIFEST_OK);
    CHECK(with("{\"config\": \"development\", \"hardware\": \"v2\", \"devices\"",
               "{\"config\": \"development\", \"hardware\": \"v2\", \"issued\": "
               "\"2026-11-23T13:51:07Z\", \"devices\"", why) == MANIFEST_OK);

    printf("malformed: refused whole\n");
    CHECK(with("\"format\": 2", "\"format\": 1", why) == MANIFEST_INVALID && strstr(why, "format 1"));
    CHECK(with("\"format\": 2,", "", why) == MANIFEST_INVALID);
    CHECK(with("\"configurations\"", "\"Configurations\"", why) == MANIFEST_INVALID);
    CHECK(with("{\"hardware\": \"v3\",", "{\"hardwar\": \"v3\",", why) == MANIFEST_INVALID &&
          strstr(why, "configuration 3") != NULL);
    CHECK(with("\"version\": \"1.0.0\"", "\"version\": \"1.0\"", why) == MANIFEST_OK);
    CHECK(with("\"version\": \"1.0.0\"", "\"version\": \"1.0.x\"", why) == MANIFEST_INVALID);
    CHECK(with("\"version\": \"1.0.0\"", "\"version\": 1", why) == MANIFEST_INVALID);
    CHECK(with("https://rubidium.darksky.io/v3", "ftp://rubidium.darksky.io/v3", why) ==
              MANIFEST_INVALID &&
          strstr(why, "URL") != NULL);
    CHECK(with("\"size\": 7900", "\"size\": 0", why) == MANIFEST_INVALID && strstr(why, "size"));
    CHECK(with("\"size\": 7900", "\"size\": -1", why) == MANIFEST_INVALID);
    CHECK(with("\"size\": 7900", "\"size\": \"7900\"", why) == MANIFEST_INVALID);
    CHECK(with(SHA "\"}}", "abc\"}}", why) == MANIFEST_INVALID && strstr(why, "SHA-256"));
    CHECK(with("[\"ec:64:c9:06:d3:50\"]", "\"ec:64:c9:06:d3:50\"", why) == MANIFEST_INVALID);
    CHECK(with("[\"ec:64:c9:06:d3:50\"]", "[5]", why) == MANIFEST_INVALID);
    CHECK(with("\"config\": \"development\", \"hardware\": \"v2\",\n",
               "\"config\": 3, \"hardware\": \"v2\",\n", why) == MANIFEST_INVALID);
    CHECK(with("{\"hardware\": \"v3\",\n     \"esp\": {\"version\": \"1.0.0\", \"url\": "
               "\"https://rubidium.darksky.io/v3/esp.bin\", \"size\": 1, \"sha256\": \"" SHA "\"}},",
               "{\"hardware\": \"v3\"},", why) == MANIFEST_INVALID &&
          strstr(why, "nothing") != NULL);

    printf("versions\n");
    CHECK(manifest_compare_versions("0.4.0", "0.3.10", &order) && order == 1);
    CHECK(manifest_compare_versions("0.3.10", "0.3.9", &order) && order == 1);
    CHECK(manifest_compare_versions("1.0", "1.0.0", &order) && order == 0);
    CHECK(manifest_compare_versions("0.04.0", "0.4.0", &order) && order == 0);
    CHECK(!manifest_compare_versions("0.4.", "0.4.0", &order));

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
