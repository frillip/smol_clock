#ifndef WIFI_H_STUB
#define WIFI_H_STUB
#include "Arduino.h"
struct FakeSta
{
    bool global_ipv6 = true;
    bool hasGlobalIPv6() const { return global_ipv6; }
};
struct FakeWiFi
{
    FakeSta STA;
};
extern FakeWiFi WiFi;
#endif
