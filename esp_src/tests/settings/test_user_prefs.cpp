/*
 * user_prefs against a fake NVS: settings saved under another header
 * (older firmware) replaced by the defaults, NTP servers 2 to 4 from the
 * defaults when not saved, and a saved choice of none kept.
 */
#include <stdio.h>
#include <string.h>

#include <Preferences.h>

#include "../../src/settings/user_prefs.h"
#include "../../src/timing/temp_comp.h"

std::map<std::string, std::vector<unsigned char>> fake_nvs;
uint32_t fake_millis;

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

static uint32_t stored_header(const char *key)
{
    uint32_t header;
    memcpy(&header, fake_nvs[key].data(), 4);
    return header;
}

int main(void)
{
    Stream out;

    printf("Nothing stored: the defaults\n");
    user_prefs_init(&out);
    CHECK(out.out.find("none stored (0 bytes)") != std::string::npos);
    CHECK(strcmp(user_prefs_ntp_server(0), "rubidium.darksky.io") == 0);
    CHECK(strcmp(user_prefs_ntp_server(1), "time.cloudflare.com") == 0);
    CHECK(strcmp(user_prefs_ntp_server(2), "ntp1.npl.co.uk") == 0);
    CHECK(user_prefs_ntp_server(3)[0] == '\0');
    CHECK(stored_header("PREF_STRUCT") == USER_PREFS_HEADER_VALUE &&
          stored_header("NTP_SERVERS") == USER_PREFS_HEADER_VALUE);

    printf("Saved by V2, or V2+ up to 0.3.0: a clean slate\n");
    fake_nvs.clear();
    user_prefs_reset();
    memcpy(user_prefs.raw, "\xEE\xEE\xFF\xC0", 4); // 0xC0FFEEEE
    user_prefs_set_string(user_prefs.fields.ntp.server, "ntp.example");
    user_prefs.fields.telnet.port = 2323;
    fake_nvs["PREF_STRUCT"].assign(user_prefs.raw, user_prefs.raw + sizeof(user_prefs.raw));
    memset(&user_prefs, 0, sizeof(user_prefs));
    out.out.clear();
    user_prefs_init(&out);
    CHECK(out.out.find("from other firmware, defaults saved") != std::string::npos);
    CHECK(strcmp(user_prefs_ntp_server(0), "rubidium.darksky.io") == 0);
    CHECK(user_prefs.fields.telnet.port == TELNET_PORT_DEFAULT);
    CHECK(stored_header("PREF_STRUCT") == USER_PREFS_HEADER_VALUE && !user_prefs_unsaved());
    CHECK(memcmp(fake_nvs["PREF_STRUCT"].data(), "LOMS", 4) == 0); // little-endian "SMOL"

    printf("Servers 2 to 4 cleared and saved: kept\n");
    user_prefs_set_string(user_prefs_ntp_server(1), "");
    user_prefs_set_string(user_prefs_ntp_server(2), "");
    user_prefs_save();
    memset(&user_prefs, 0, sizeof(user_prefs));
    memset(&user_prefs_ntp, 0, sizeof(user_prefs_ntp));
    user_prefs_init(&out);
    CHECK(strcmp(user_prefs_ntp_server(0), "rubidium.darksky.io") == 0);
    CHECK(user_prefs_ntp_server(1)[0] == '\0' && user_prefs_ntp_server(2)[0] == '\0');

    printf("Servers 2 to 4 not saved: the defaults\n");
    fake_nvs.erase("NTP_SERVERS");
    user_prefs_init(&out);
    CHECK(strcmp(user_prefs_ntp_server(1), "time.cloudflare.com") == 0);
    CHECK(stored_header("NTP_SERVERS") == USER_PREFS_HEADER_VALUE);

    printf("The format 1 manifest's path: the format 2 one, saved; one set by hand kept\n");
    CHECK(strcmp(user_prefs.fields.updater.path, "/smol_clock/v2plus/manifest.json") == 0);
    user_prefs_set_string(user_prefs.fields.updater.path, "/smol_clock/v2plus/update.json");
    user_prefs_save();
    memset(&user_prefs, 0, sizeof(user_prefs));
    out.out.clear();
    user_prefs_init(&out);
    CHECK(strcmp(user_prefs.fields.updater.path, "/smol_clock/v2plus/manifest.json") == 0);
    CHECK(out.out.find("now /smol_clock/v2plus/manifest.json") != std::string::npos);
    CHECK(!user_prefs_unsaved());
    user_prefs_set_string(user_prefs.fields.updater.path, "/mine/manifest.json");
    user_prefs_save();
    memset(&user_prefs, 0, sizeof(user_prefs));
    user_prefs_init(&out);
    CHECK(strcmp(user_prefs.fields.updater.path, "/mine/manifest.json") == 0);

    printf("The NTP asymmetry: 0 if never stored, kept, repaired if out of range\n");
    fake_nvs.erase("NTP_ASYM");
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_asymmetry_us == 0 && !user_prefs_unsaved());
    user_prefs_ntp_asymmetry_us = 120;
    CHECK(user_prefs_unsaved());
    user_prefs_save();
    user_prefs_ntp_asymmetry_us = 0;
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_asymmetry_us == 120 && !user_prefs_unsaved());
    {
        int32_t bad = NTP_ASYMMETRY_MAX_US + 1;
        fake_nvs["NTP_ASYM"].assign((unsigned char *)&bad, (unsigned char *)&bad + 4);
    }
    out.out.clear();
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_asymmetry_us == 0 && out.out.find("ntp.asymmetry invalid") != std::string::npos);
    user_prefs_print(&out);
    CHECK(out.out.find("asymmetry +0 us") != std::string::npos);

    printf("Crystal temperature compensation: on, nothing set or learnt, if never stored; kept; "
           "repaired\n");
    fake_nvs.erase("ESP_TCOMP");
    user_prefs_init(&out);
    CHECK(user_prefs_esp_tcomp.enabled == 1 &&
          user_prefs_esp_tcomp.set_ppb_per_c == TEMP_COMP_NOT_SET &&
          user_prefs_esp_tcomp.learnt_estimates == 0 && !user_prefs_unsaved());
    user_prefs_esp_tcomp.enabled = 0;
    user_prefs_esp_tcomp.set_ppb_per_c = -120;
    CHECK(user_prefs_unsaved());
    user_prefs_save();
    user_prefs_init(&out);
    CHECK(user_prefs_esp_tcomp.enabled == 0 && user_prefs_esp_tcomp.set_ppb_per_c == -120 &&
          !user_prefs_unsaved());

    printf("  What is learnt saves itself, and only itself; the defaults keep it\n");
    user_prefs_esp_tcomp.enabled = 1; // not saved
    user_prefs_esp_tcomp.learnt_ppb_per_c = -187;
    user_prefs_esp_tcomp.learnt_estimates = 4;
    user_prefs_save_tcomp_learnt();
    CHECK(user_prefs_unsaved()); // the enable still
    user_prefs_init(&out);
    CHECK(user_prefs_esp_tcomp.enabled == 0 && user_prefs_esp_tcomp.set_ppb_per_c == -120 &&
          user_prefs_esp_tcomp.learnt_ppb_per_c == -187 &&
          user_prefs_esp_tcomp.learnt_estimates == 4 && !user_prefs_unsaved());
    user_prefs_reset();
    CHECK(user_prefs_esp_tcomp.enabled == 1 &&
          user_prefs_esp_tcomp.set_ppb_per_c == TEMP_COMP_NOT_SET &&
          user_prefs_esp_tcomp.learnt_ppb_per_c == -187 &&
          user_prefs_esp_tcomp.learnt_estimates == 4);
    user_prefs_print(&out);
    CHECK(out.out.find("temperature compensation: ON, learnt -187 ppb/C (4 estimates)") !=
          std::string::npos);
    user_prefs_save();

    printf("  The old two-value record: defaults; out of range: repaired\n");
    {
        int32_t old[2] = {1, -200};
        fake_nvs["ESP_TCOMP"].assign((unsigned char *)old, (unsigned char *)old + 8);
    }
    user_prefs_init(&out);
    CHECK(user_prefs_esp_tcomp.enabled == 1 &&
          user_prefs_esp_tcomp.set_ppb_per_c == TEMP_COMP_NOT_SET &&
          user_prefs_esp_tcomp.learnt_estimates == 0);
    {
        int32_t bad[4] = {1, TEMP_COMP_MAX_PPB_PER_C + 1, 0, 0};
        fake_nvs["ESP_TCOMP"].assign((unsigned char *)bad, (unsigned char *)bad + 16);
    }
    out.out.clear();
    user_prefs_init(&out);
    CHECK(user_prefs_esp_tcomp.set_ppb_per_c == TEMP_COMP_NOT_SET &&
          out.out.find("esp.tcomp invalid") != std::string::npos);
    user_prefs_print(&out);
    CHECK(out.out.find("temperature compensation: ON\n") != std::string::npos);

    printf("NTP offsets: none if never stored, kept, repaired if out of range\n");
    fake_nvs.erase("NTP_OFFS");
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_offset_us[0] == 0 && user_prefs_ntp_offset_us[NTP_SERVERS] == 0 &&
          !user_prefs_unsaved());
    user_prefs_ntp_offset_us[0] = 120;
    user_prefs_ntp_offset_us[3] = -450;
    CHECK(user_prefs_unsaved());
    user_prefs_save();
    memset(user_prefs_ntp_offset_us, 0, sizeof(user_prefs_ntp_offset_us));
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_offset_us[0] == 120 && user_prefs_ntp_offset_us[3] == -450 &&
          !user_prefs_unsaved());
    out.out.clear();
    user_prefs_print(&out);
    CHECK(out.out.find("; 0 offset +120 us; 3 offset -450 us\n") != std::string::npos);
    user_prefs_ntp_offset_us[3] = NTP_OFFSET_MAX_US + 1;
    user_prefs_save();
    out.out.clear();
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_offset_us[3] == 0 && user_prefs_ntp_offset_us[0] == 120 &&
          out.out.find("ntp.offset invalid") != std::string::npos);
    user_prefs_ntp_offset_us[0] = 0;
    user_prefs_save();

    printf("DHCP's NTP server: enabled if never stored, kept, repaired if out of range\n");
    fake_nvs.erase("NTP_DHCP");
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_ENABLED && !user_prefs_unsaved());
    user_prefs_ntp_dhcp = NTP_DHCP_PREFERRED;
    CHECK(user_prefs_unsaved());
    user_prefs_save();
    user_prefs_ntp_dhcp = NTP_DHCP_DISABLED;
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_PREFERRED);
    {
        int32_t bad = 7;
        fake_nvs["NTP_DHCP"].assign((unsigned char *)&bad, (unsigned char *)&bad + 4);
    }
    out.out.clear();
    user_prefs_init(&out);
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_ENABLED && out.out.find("ntp.dhcp invalid") != std::string::npos);
    out.out.clear();
    user_prefs_print(&out);
    CHECK(out.out.find("0: DHCP's, enable; 1: ") != std::string::npos);

    printf("Anything else: the defaults\n");
    user_prefs.fields.header = 0x12345678;
    user_prefs_save();
    out.out.clear();
    user_prefs_init(&out);
    CHECK(out.out.find("from other firmware") != std::string::npos);
    CHECK(strcmp(user_prefs_ntp_server(0), "rubidium.darksky.io") == 0);
    CHECK(strcmp(user_prefs_ntp_server(2), "ntp1.npl.co.uk") == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
