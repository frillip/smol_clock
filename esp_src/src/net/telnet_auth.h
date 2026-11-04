/*
 * The telnet console's password. Telnet is plaintext, so this keeps out the
 * casual, not the determined: the password crosses the network as typed.
 *
 *   telnet passwd <password>  sets it (a protected command; always from
 *                             the USB serial console, which needs no login)
 *   telnet login <password>   authorises this telnet connection until it
 *                             closes
 *
 * Until a connection logs in, commands that change anything are refused on
 * it (commands.cpp says which). The password is kept salted and hashed
 * (SHA-256) in NVS; until one is set, TELNET_PASSWORD_DEFAULT stands.
 *
 * TELNET_LOGIN_GRACE wrong passwords in a row, then each further one locks
 * logins for TELNET_LOCKOUT_MS: commands run as normal meanwhile, only login
 * is refused. Shared by all connections, so reconnecting gains nothing. A
 * good login or a password change starts the count again.
 *
 * Portable: storage and randomness are the platform's (telnet_auth_load,
 * _save, _random), so the host tests run the rest.
 */
#ifndef TELNET_AUTH_H
#define TELNET_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#include "../util/sha256.h"

#define TELNET_PASSWORD_DEFAULT "smolclock"
#define TELNET_PASSWORD_MIN     4U
#define TELNET_PASSWORD_MAX     64U
#define TELNET_LOGIN_GRACE      5U
#define TELNET_LOCKOUT_MS       30000U

#define TELNET_AUTH_FORMAT 1U
#define TELNET_SALT_BYTES  16U

typedef struct
{
    uint8_t format;
    uint8_t salt[TELNET_SALT_BYTES];
    uint8_t hash[SHA256_BYTES]; // SHA-256 of the salt, then the password
} TelnetAuthRecord;

typedef enum
{
    TELNET_LOGIN_OK,
    TELNET_LOGIN_WRONG,  // detail: attempts left before the lockout, 0 if now locked
    TELNET_LOGIN_LOCKED, // detail: seconds until a login is taken again
} TelnetLogin;

// At start-up: the stored password, else the default.
void telnet_auth_init(void);

TelnetLogin telnet_auth_login(const char *password, uint32_t now_ms, uint32_t *detail);

// A new password, stored. False if its length is outside
// TELNET_PASSWORD_MIN..MAX. Starts the wrong-password count again.
bool telnet_auth_set(const char *password);

// The default password still stands.
bool telnet_auth_is_default(void);
// Wrong passwords since the last good login or change.
uint32_t telnet_auth_failures(void);

// The platform's (telnet_auth_nvs.cpp on the ESP).
bool telnet_auth_load(TelnetAuthRecord *record);
void telnet_auth_save(const TelnetAuthRecord *record);
void telnet_auth_random(uint8_t *data, uint32_t length);

#endif // TELNET_AUTH_H
