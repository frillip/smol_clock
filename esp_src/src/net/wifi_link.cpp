#include "wifi_link.h"

#include "dhcp_ntp.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "../console/console.h"
#include "net_connect.h"

extern "C"
{
#include "../pic/sml/sml_proto.h"
}


#define RECONNECT_INTERVAL_MS 30000U
#define PORTAL_TIMEOUT_S      900

static WiFiManager manager;
static char hostname[24];
static uint32_t last_attempt_ms;
static uint32_t reconnects;
// The addresses as last printed.
static bool shown_up;
static IPAddress shown_ipv4;
static IPAddress shown_ipv6(IPv6);
static bool was_up;

/*
 * No Wi-Fi power save, for timing. With modem sleep (arduino-esp32's
 * default) the radio sleeps between the access point's beacons, and a
 * frame for the clock waits at the access point for the next one it wakes
 * for: tens to hundreds of milliseconds, on the way in only. That delay
 * lands in the round trip and the asymmetry of every NTP exchange, and
 * would land in the receive time of any request the clock answered. The
 * cost is the radio's receive current all the time, some tens of mA, from
 * a clock on mains power.
 */
static void power_save_off(void)
{
    wifi_ps_type_t mode;

    // Before the station starts, this is kept and applied when it does (each
    // start, the setup access point's included); after, applied at once.
    WiFi.setSleep(WIFI_PS_NONE);
    if (esp_wifi_get_ps(&mode) == ESP_OK && mode != WIFI_PS_NONE)
    {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}


void wifi_link_init(Stream *output)
{
    uint64_t mac = ESP.getEfuseMac();
    uint32_t id = 0;
    int index;

    // The last three bytes of the MAC, as on V2.
    for (index = 0; index < 17; index += 8)
    {
        id |= ((mac >> (40 - index)) & 0xFF) << index;
    }
    snprintf(hostname, sizeof(hostname), "SMOL_CLOCK_%06lX", (unsigned long)(id & 0xFFFFFF));
    WiFi.setHostname(hostname);
    output->printf("%s\n", hostname);

    power_save_off();
    WiFi.mode(WIFI_STA);
    WiFi.enableIPv6(); // SLAAC once connected
    dhcp_ntp_init(); // before the first lease
    manager.setConfigPortalBlocking(false);
    manager.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
    manager.setConnectTimeout(5);
    if (manager.autoConnect(hostname))
    {
        output->printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
        shown_up = true;
        shown_ipv4 = WiFi.localIP();
    }
    else
    {
        output->printf("Wi-Fi setup access point %s running\n", hostname);
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    }
}


// A change of address, printed: the global IPv6 address comes a few
// seconds after IPv4, with the router's first advertisement.
static void follow_addresses(void)
{
    bool up = wifi_link_is_up();
    IPAddress ipv4 = up ? WiFi.localIP() : IPAddress();
    IPAddress ipv6(IPv6);

    if (up)
    {
        (void)wifi_link_ipv6(&ipv6);
    }
    if (up == shown_up && ipv4 == shown_ipv4 && ipv6 == shown_ipv6)
    {
        return;
    }
    shown_up = up;
    shown_ipv4 = ipv4;
    shown_ipv6 = ipv6;
    if (up)
    {
        console_printf("Wi-Fi: %s", ipv4.toString().c_str());
        if (ipv6 != IPAddress(IPv6))
        {
            console_printf(", %s", ipv6.toString().c_str());
        }
        console_printf("\n");
    }
}


void wifi_link_service(void)
{
    bool up;

    manager.process();
    follow_addresses();
    up = WiFi.status() == WL_CONNECTED;
    // Each new connection: still no power save, whatever restarted the
    // station in between (WiFiManager's setup access point, say).
    if (up && !was_up)
    {
        power_save_off();
    }
    was_up = up;
    if (up || !manager.getWiFiIsSaved() || manager.getConfigPortalActive())
    {
        return;
    }
    if (millis() - last_attempt_ms > RECONNECT_INTERVAL_MS)
    {
        last_attempt_ms = millis();
        if (reconnects++ == 0)
        {
            Serial.println("Reconnecting to Wi-Fi...");
        }
        WiFi.disconnect();
        WiFi.reconnect();
    }
}


bool wifi_link_is_up(void)
{
    return WiFi.status() == WL_CONNECTED;
}


bool wifi_link_ipv6(IPAddress *address)
{
    if (!WiFi.STA.hasGlobalIPv6())
    {
        return false;
    }
    *address = WiFi.globalIPv6();
    return *address != IPAddress(IPv6);
}


void wifi_link_print_addresses(Stream *output, uint16_t port)
{
    IPAddress ipv6;

    if (!wifi_link_is_up())
    {
        return;
    }
    output->printf("%s", WiFi.localIP().toString().c_str());
    if (port != 0)
    {
        output->printf(":%u", port);
    }
    if (wifi_link_ipv6(&ipv6))
    {
        if (port != 0)
        {
            output->printf(", [%s]:%u", ipv6.toString().c_str(), port);
        }
        else
        {
            output->printf(", %s", ipv6.toString().c_str());
        }
    }
}


static const char *ipv6_type_name(esp_ip6_addr_type_t type)
{
    switch (type)
    {
        case ESP_IP6_ADDR_IS_GLOBAL: return "GLOBAL";
        case ESP_IP6_ADDR_IS_LINK_LOCAL: return "LINK_LOCAL";
        case ESP_IP6_ADDR_IS_SITE_LOCAL: return "SITE_LOCAL";
        case ESP_IP6_ADDR_IS_UNIQUE_LOCAL: return "UNIQUE_LOCAL";
        default: return "OTHER";
    }
}


// Every IPv6 address the interface has, with its kind.
static void print_ipv6_all(Stream *output)
{
    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    esp_netif_t *netif = WiFi.STA.netif();
    int count = netif != NULL ? esp_netif_get_all_ip6(netif, addresses) : 0;
    int index;

    output->printf("IPv6:  ");
    if (count <= 0)
    {
        output->printf(" none\n");
        return;
    }
    for (index = 0; index < count; index++)
    {
        IPAddress address(IPv6, (const uint8_t *)addresses[index].addr, addresses[index].zone);

        output->printf("%s %s %s\n", index == 0 ? "" : "       ", address.toString().c_str(),
                       ipv6_type_name(esp_netif_ip6_get_addr_type(&addresses[index])));
    }
}


// The DNS servers in use, IPv4 or IPv6, from DHCP or RDNSS.
static void print_dns(Stream *output)
{
    uint8_t index;
    bool any = false;

    output->printf("DNS:    ");
    for (index = 0; index < ESP_NETIF_DNS_MAX; index++)
    {
        IPAddress server = WiFi.dnsIP(index);

        if (server == IPAddress() || server == IPAddress(IPv6))
        {
            continue;
        }
        output->printf("%s%s", any ? ", " : "", server.toString().c_str());
        any = true;
    }
    output->printf("%s\n", any ? "" : "none");
}


uint8_t wifi_link_state(void)
{
    if (manager.getConfigPortalActive())
    {
        return SML_WIFI_PROVISIONING;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        return SML_WIFI_CONNECTED;
    }
    if (!manager.getWiFiIsSaved())
    {
        return SML_WIFI_NOT_CONFIGURED;
    }
    return WiFi.status() == WL_IDLE_STATUS ? SML_WIFI_CONNECTING : SML_WIFI_DISCONNECTED;
}


const char *wifi_link_hostname(void)
{
    return hostname;
}


void wifi_link_forget(void)
{
    manager.resetSettings();
}


void wifi_link_clear(Stream *output)
{
    output->printf("Clearing the Wi-Fi settings and restarting\n");
    output->flush(); // a telnet reply may still be queued
    wifi_link_forget();
    delay(100);
    ESP.restart();
}


void wifi_link_setup(Stream *output)
{
    if (manager.getConfigPortalActive())
    {
        output->printf("Wi-Fi setup access point already running\n");
        return;
    }
    output->printf("Wi-Fi setup access point %s starting\n", hostname);
    manager.startConfigPortal(hostname);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
}


void wifi_link_reconnect(void)
{
    WiFi.reconnect();
}


void wifi_link_disconnect(void)
{
    WiFi.disconnect();
}


static const char *power_save_name(void)
{
    wifi_ps_type_t mode;

    if (esp_wifi_get_ps(&mode) != ESP_OK)
    {
        return "?";
    }
    switch (mode)
    {
        case WIFI_PS_NONE: return "OFF (no power save, for timing)";
        case WIFI_PS_MIN_MODEM: return "MIN_MODEM";
        case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
        default: return "?";
    }
}


void wifi_link_print(Stream *output, bool verbose)
{
    bool up = wifi_link_is_up();

    if (!verbose)
    {
        output->printf("Wi-Fi:    %s", up ? "" : "disconnected, ");
        output->printf("%s", manager.getWiFiIsSaved() ? manager.getWiFiSSID(true).c_str()
                                                      : "no network saved");
        if (up)
        {
            output->printf(", ");
            wifi_link_print_addresses(output, 0);
            output->printf(", %d dBm", (int)WiFi.RSSI());
        }
        output->printf("%s\n",
                       manager.getConfigPortalActive() ? ", setup access point active" : "");
        return;
    }

    output->printf("Wi-Fi:  %s\n", up ? "CONNECTED" : "DISCONNECTED");
    output->printf("SSID:   %s\n",
                   manager.getWiFiIsSaved() ? manager.getWiFiSSID(true).c_str() : "(none)");
    if (up)
    {
        output->printf("IPv4:   %s, gateway %s\n", WiFi.localIP().toString().c_str(),
                       WiFi.gatewayIP().toString().c_str());
        print_ipv6_all(output);
        print_dns(output);
        net_connect_print(output);
        output->printf("Signal: %d dBm, channel %d\n", (int)WiFi.RSSI(), (int)WiFi.channel());
    }
    output->printf("Sleep:  %s\n", power_save_name());
    if (manager.getConfigPortalActive())
    {
        output->printf("Setup access point %s active\n", hostname);
    }
    output->printf("MAC:    %s\n", WiFi.macAddress().c_str());
}


static const char *auth_name(wifi_auth_mode_t mode)
{
    switch (mode)
    {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3-PSK";
        case WIFI_AUTH_ENTERPRISE: return "WPA-EAP";
        default: return "other";
    }
}


void wifi_link_scan(Stream *output)
{
    String current = WiFi.SSID();
    int count;
    int index;

    output->printf("Scanning...\n");
    count = WiFi.scanNetworks();
    if (count <= 0)
    {
        output->printf("No networks found\n");
        return;
    }
    for (index = 0; index < count; index++)
    {
        bool connected = wifi_link_is_up() && WiFi.SSID(index) == current;

        output->printf(" %-32s %4d dBm  %-8s%s\n", WiFi.SSID(index).c_str(), (int)WiFi.RSSI(index),
                       auth_name(WiFi.encryptionType(index)), connected ? "  (connected)" : "");
    }
    WiFi.scanDelete();
}
