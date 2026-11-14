/*
 * The NTP server DHCP offers (option 42): lwIP, as this core builds it
 * (CONFIG_LWIP_DHCP_GET_NTP_SRV, one server), asks for it with every lease
 * and hands it to its SNTP module, which keeps it only if told to. The
 * SNTP client itself never runs: only its server list is read. IPv4 only
 * (no DHCPv6 NTP option in this lwIP).
 */
#ifndef DHCP_NTP_H
#define DHCP_NTP_H

#include <Arduino.h>

// Once the network interface exists (after WiFi.mode()), before the first
// lease: keep the server DHCP offers.
void dhcp_ntp_init(void);
// The server the latest lease offered. False if none.
bool dhcp_ntp_server(IPAddress *address);

#endif // DHCP_NTP_H
