// telnet_auth.h's storage on the ESP: NVS namespace "telnet", key "auth".
#include <Preferences.h>
#include <esp_random.h>

#include "telnet_auth.h"

#define AUTH_NAMESPACE "telnet"
#define AUTH_KEY       "auth"

bool telnet_auth_load(TelnetAuthRecord *record)
{
    Preferences store;
    bool found;

    store.begin(AUTH_NAMESPACE, true);
    found = store.getBytesLength(AUTH_KEY) == sizeof(*record) &&
            store.getBytes(AUTH_KEY, record, sizeof(*record)) == sizeof(*record);
    store.end();
    return found;
}


void telnet_auth_save(const TelnetAuthRecord *record)
{
    Preferences store;

    store.begin(AUTH_NAMESPACE, false);
    store.putBytes(AUTH_KEY, record, sizeof(*record));
    store.end();
}


void telnet_auth_random(uint8_t *data, uint32_t length)
{
    esp_fill_random(data, length);
}
