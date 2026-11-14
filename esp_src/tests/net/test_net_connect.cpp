/*
 * Dual-stack fallback (src/net/net_connect.cpp) against a table resolver and
 * stand-in clients: the order addresses are tried in, IPv6 only with a
 * global address of our own, a failed family held back for an hour (and
 * given back once the other fails too), literals (IPv6 bracketed or not)
 * and URLs.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/net/net_connect.h"
#include <WiFi.h>
#include <lwip/netdb.h>

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

uint32_t fake_millis = 1000;
FakeWiFi WiFi;
std::vector<std::string> fake_reachable;
std::vector<std::string> fake_tried;
std::string fake_sni;
uint16_t fake_port;

// The names: dual has both, four only IPv4, six only IPv6.
static const struct
{
    const char *name;
    const char *ipv6;
    const char *ipv4;
} names[] = {
    {"dual.example", "2001:db8::1", "192.0.2.1"},
    {"four.example", NULL, "192.0.2.4"},
    {"six.example", "2001:db8::6", NULL},
};
static int lookups;

int lwip_getaddrinfo(const char *name, const char *service, const struct addrinfo *hints,
                     struct addrinfo **result)
{
    (void)service;
    lookups++;
    for (const auto &entry : names)
    {
        if (strcmp(entry.name, name) != 0)
            continue;
        const char *address = hints->ai_family == AF_INET6 ? entry.ipv6 : entry.ipv4;
        if (address == NULL)
            return EAI_NONAME;
        struct addrinfo *info = (struct addrinfo *)calloc(1, sizeof(*info));
        if (hints->ai_family == AF_INET6)
        {
            struct sockaddr_in6 *six = (struct sockaddr_in6 *)calloc(1, sizeof(*six));
            six->sin6_family = AF_INET6;
            strncpy((char *)six->sin6_addr.s6_addr, address, 15); // the stub's text
            info->ai_family = AF_INET6;
            info->ai_addr = (struct sockaddr *)six;
        }
        else
        {
            struct sockaddr_in *four = (struct sockaddr_in *)calloc(1, sizeof(*four));
            four->sin_family = AF_INET;
            inet_pton(AF_INET, address, &four->sin_addr);
            info->ai_family = AF_INET;
            info->ai_addr = (struct sockaddr *)four;
        }
        *result = info;
        return 0;
    }
    return EAI_NONAME;
}


void lwip_freeaddrinfo(struct addrinfo *info)
{
    free(info->ai_addr);
    free(info);
}


static std::string order(const char *host)
{
    IPAddress addresses[NET_ADDRESSES];
    int count = net_resolve(host, addresses);
    std::string text;

    for (int i = 0; i < count; i++)
        text += std::string(i ? " " : "") + addresses[i].toString().c_str();
    return text;
}


static bool connect_url(const char *url, IPAddress *used)
{
    NetworkClientSecure secure;
    NetworkClient plain;

    fake_tried.clear();
    return net_http_connect(secure, plain, url, "CA", 1000, used);
}


static std::string tried(void)
{
    std::string text;
    for (size_t i = 0; i < fake_tried.size(); i++)
        text += std::string(i ? " " : "") + fake_tried[i];
    return text;
}


int main(void)
{
    IPAddress used;
    Stream out;

    printf("Order\n");
    CHECK(order("dual.example") == "2001:db8::1 192.0.2.1");
    CHECK(order("four.example") == "192.0.2.4");
    CHECK(order("six.example") == "2001:db8::6");
    CHECK(order("none.example") == "");
    CHECK(order("192.0.2.9") == "192.0.2.9" && order("2001:db8::9") == "2001:db8::9");
    // An IPv6 literal may be bracketed, as in a URL; nothing else may.
    CHECK(order("[2001:db8::9]") == "2001:db8::9");
    CHECK(order("[192.0.2.9]") == "" && order("[2001:db8::9") == "" && order("[dual.example]") == "");
    WiFi.STA.global_ipv6 = false; // no global IPv6 address: IPv6 unreachable
    CHECK(order("dual.example") == "192.0.2.1" && order("six.example") == "");
    WiFi.STA.global_ipv6 = true;

    printf("Fallback\n");
    fake_reachable = {"192.0.2.1"}; // IPv6 does not route
    CHECK(connect_url("https://dual.example/path", &used));
    CHECK(tried() == "2001:db8::1 192.0.2.1" && used.toString() == "192.0.2.1");
    CHECK(fake_sni == "dual.example" && fake_port == 443);
    // IPv6 is now held back: IPv4 first, for an hour.
    CHECK(order("dual.example") == "192.0.2.1 2001:db8::1");
    CHECK(connect_url("https://dual.example/", &used) && tried() == "192.0.2.1");
    fake_millis += NET_FAMILY_HOLD_MS - 1000U;
    CHECK(order("dual.example") == "192.0.2.1 2001:db8::1");
    fake_millis += 1000U;
    CHECK(order("dual.example") == "2001:db8::1 192.0.2.1");
    // IPv6 fails again; then IPv4 fails too, later: IPv6 back in front.
    CHECK(connect_url("https://dual.example/", &used) && tried() == "2001:db8::1 192.0.2.1");
    fake_millis += 5000U;
    fake_reachable = {"2001:db8::1"};
    CHECK(connect_url("https://dual.example/", &used) && tried() == "192.0.2.1 2001:db8::1");
    CHECK(used.toString() == "2001:db8::1");
    CHECK(order("dual.example") == "2001:db8::1 192.0.2.1");
    // IPv4 down, IPv6 up: the other way round works too.
    CHECK(connect_url("https://dual.example/", &used) && tried() == "2001:db8::1");
    fake_reachable = {};
    CHECK(!connect_url("https://dual.example/", &used) && tried() == "2001:db8::1 192.0.2.1");
    // Only an NTP poll's word: no reply on IPv4 holds IPv4 back.
    fake_millis += NET_FAMILY_HOLD_MS;
    IPAddress four;
    four.fromString("192.0.2.1");
    net_note_result(four, false);
    CHECK(order("dual.example") == "2001:db8::1 192.0.2.1");
    IPAddress six;
    six.fromString("2001:db8::1");
    net_note_result(six, false);
    CHECK(order("dual.example") == "192.0.2.1 2001:db8::1"); // IPv6 failed last
    net_note_result(four, true);
    CHECK(order("dual.example") == "192.0.2.1 2001:db8::1");
    net_connect_print(&out);
    printf("  %s", out.out.c_str());

    printf("URLs\n");
    fake_millis += NET_FAMILY_HOLD_MS;
    fake_reachable = {"192.0.2.1", "2001:db8::9", "192.0.2.9"};
    CHECK(connect_url("http://dual.example:8080/x", &used) && fake_port == 8080 && fake_sni == "");
    CHECK(connect_url("https://[2001:db8::9]:8443/x", &used) && tried() == "2001:db8::9" &&
          fake_port == 8443 && fake_sni == "2001:db8::9");
    CHECK(connect_url("https://192.0.2.9/x", &used) && tried() == "192.0.2.9");
    CHECK(!connect_url("ftp://dual.example/", &used) && tried() == "");
    CHECK(!connect_url("https:///x", &used) && !connect_url("https://[2001:db8::9/x", &used));
    CHECK(!connect_url("https://dual.example:0/", &used));
    CHECK(strcmp(net_url_host("2001:db8::9").c_str(), "[2001:db8::9]") == 0);
    CHECK(strcmp(net_url_host("dual.example").c_str(), "dual.example") == 0);
    CHECK(strcmp(net_url_host("192.0.2.9").c_str(), "192.0.2.9") == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
