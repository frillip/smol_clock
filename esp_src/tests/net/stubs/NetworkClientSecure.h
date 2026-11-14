#ifndef NETWORKCLIENTSECURE_H_STUB
#define NETWORKCLIENTSECURE_H_STUB
#include "Arduino.h"
#include <vector>

// Which addresses answer, and what was tried.
extern std::vector<std::string> fake_reachable;
extern std::vector<std::string> fake_tried;
extern std::string fake_sni;
extern uint16_t fake_port;

static inline bool fake_connect(const IPAddress &ip, uint16_t port)
{
    std::string address = ip.toString().c_str();
    fake_tried.push_back(address);
    fake_port = port;
    for (const std::string &reachable : fake_reachable)
        if (reachable == address)
            return true;
    return false;
}

class NetworkClient
{
  public:
    int connect(IPAddress ip, uint16_t port, int32_t timeout)
    {
        (void)timeout;
        fake_sni = "";
        return fake_connect(ip, port) ? 1 : 0;
    }
};

class NetworkClientSecure : public NetworkClient
{
  public:
    void setConnectionTimeout(uint32_t ms) { (void)ms; }
    void setCACert(const char *ca) { (void)ca; }
    int connect(IPAddress ip, uint16_t port, const char *host, const char *ca, const char *cert,
                const char *key)
    {
        (void)ca;
        (void)cert;
        (void)key;
        fake_sni = host;
        return fake_connect(ip, port) ? 1 : 0;
    }
};
#endif
