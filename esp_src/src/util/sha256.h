/*
 * SHA-256 (FIPS 180-4), portable C99, for the update manifest's checksums:
 * the same code on the ESP and in the host tests.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SHA256_BYTES 32

typedef struct
{
    uint32_t state[8];
    uint64_t length; // bytes so far
    uint8_t block[64];
    uint8_t used; // bytes in block
} Sha256;

void sha256_start(Sha256 *sha);
void sha256_update(Sha256 *sha, const void *data, size_t length);
void sha256_finish(Sha256 *sha, uint8_t digest[SHA256_BYTES]);

// 64 hex digits (either case) into a digest. False if not exactly that.
int sha256_from_hex(const char *text, uint8_t digest[SHA256_BYTES]);

#ifdef __cplusplus
}
#endif

#endif // SHA256_H
