#include "telnet_auth.h"

#include <string.h>

static TelnetAuthRecord record;
static bool is_default;
static uint32_t failures;
static uint32_t locked_ms; // when the latest lockout began

static void hash_of(const uint8_t salt[TELNET_SALT_BYTES], const char *password,
                    uint8_t hash[SHA256_BYTES])
{
    Sha256 sha;

    sha256_start(&sha);
    sha256_update(&sha, salt, TELNET_SALT_BYTES);
    sha256_update(&sha, password, strlen(password));
    sha256_finish(&sha, hash);
}


// Every byte compared, whatever the first difference.
static bool same(const uint8_t *a, const uint8_t *b, uint32_t length)
{
    uint8_t difference = 0;
    uint32_t index;

    for (index = 0; index < length; index++)
    {
        difference |= (uint8_t)(a[index] ^ b[index]);
    }
    return difference == 0;
}


void telnet_auth_init(void)
{
    failures = 0;
    if (telnet_auth_load(&record) && record.format == TELNET_AUTH_FORMAT)
    {
        is_default = false;
        return;
    }
    memset(&record, 0, sizeof(record));
    record.format = TELNET_AUTH_FORMAT;
    telnet_auth_random(record.salt, TELNET_SALT_BYTES);
    hash_of(record.salt, TELNET_PASSWORD_DEFAULT, record.hash);
    is_default = true;
}


TelnetLogin telnet_auth_login(const char *password, uint32_t now_ms, uint32_t *detail)
{
    uint8_t hash[SHA256_BYTES];

    if (failures >= TELNET_LOGIN_GRACE && now_ms - locked_ms < TELNET_LOCKOUT_MS)
    {
        *detail = (TELNET_LOCKOUT_MS - (now_ms - locked_ms) + 999U) / 1000U;
        return TELNET_LOGIN_LOCKED;
    }
    hash_of(record.salt, password, hash);
    if (same(hash, record.hash, SHA256_BYTES))
    {
        failures = 0;
        *detail = 0;
        return TELNET_LOGIN_OK;
    }
    failures++;
    if (failures >= TELNET_LOGIN_GRACE)
    {
        locked_ms = now_ms;
        *detail = 0;
    }
    else
    {
        *detail = TELNET_LOGIN_GRACE - failures;
    }
    return TELNET_LOGIN_WRONG;
}


bool telnet_auth_set(const char *password)
{
    size_t length = strlen(password);

    if (length < TELNET_PASSWORD_MIN || length > TELNET_PASSWORD_MAX)
    {
        return false;
    }
    record.format = TELNET_AUTH_FORMAT;
    telnet_auth_random(record.salt, TELNET_SALT_BYTES);
    hash_of(record.salt, password, record.hash);
    telnet_auth_save(&record);
    is_default = false;
    failures = 0;
    return true;
}


bool telnet_auth_is_default(void)
{
    return is_default;
}


uint32_t telnet_auth_failures(void)
{
    return failures;
}
