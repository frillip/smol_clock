#include "sha256.h"

#include <string.h>

static const uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};


static uint32_t rotate(uint32_t value, unsigned bits)
{
    return value >> bits | value << (32U - bits);
}


static void compress(Sha256 *sha, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t v[8];
    unsigned index;

    for (index = 0; index < 16; index++)
    {
        w[index] = (uint32_t)block[4 * index] << 24 | (uint32_t)block[4 * index + 1] << 16 |
                   (uint32_t)block[4 * index + 2] << 8 | block[4 * index + 3];
    }
    for (index = 16; index < 64; index++)
    {
        uint32_t s0 = rotate(w[index - 15], 7) ^ rotate(w[index - 15], 18) ^ w[index - 15] >> 3;
        uint32_t s1 = rotate(w[index - 2], 17) ^ rotate(w[index - 2], 19) ^ w[index - 2] >> 10;

        w[index] = w[index - 16] + s0 + w[index - 7] + s1;
    }
    memcpy(v, sha->state, sizeof(v));
    for (index = 0; index < 64; index++)
    {
        uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
        uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + choose + constants[index] + w[index];
        uint32_t s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
        uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

        memmove(&v[1], &v[0], 7 * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + s0 + majority;
    }
    for (index = 0; index < 8; index++)
    {
        sha->state[index] += v[index];
    }
}


void sha256_start(Sha256 *sha)
{
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    memcpy(sha->state, initial, sizeof(initial));
    sha->length = 0;
    sha->used = 0;
}


void sha256_update(Sha256 *sha, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;

    sha->length += length;
    while (length != 0)
    {
        size_t take = 64U - sha->used;

        if (take > length)
        {
            take = length;
        }
        memcpy(sha->block + sha->used, bytes, take);
        sha->used = (uint8_t)(sha->used + take);
        bytes += take;
        length -= take;
        if (sha->used == 64U)
        {
            compress(sha, sha->block);
            sha->used = 0;
        }
    }
}


void sha256_finish(Sha256 *sha, uint8_t digest[SHA256_BYTES])
{
    uint64_t bits = sha->length * 8U;
    unsigned index;

    sha->block[sha->used++] = 0x80;
    if (sha->used > 56U)
    {
        memset(sha->block + sha->used, 0, 64U - sha->used);
        compress(sha, sha->block);
        sha->used = 0;
    }
    memset(sha->block + sha->used, 0, 56U - sha->used);
    for (index = 0; index < 8; index++)
    {
        sha->block[56 + index] = (uint8_t)(bits >> (56 - 8 * index));
    }
    compress(sha, sha->block);
    for (index = 0; index < 8; index++)
    {
        digest[4 * index] = (uint8_t)(sha->state[index] >> 24);
        digest[4 * index + 1] = (uint8_t)(sha->state[index] >> 16);
        digest[4 * index + 2] = (uint8_t)(sha->state[index] >> 8);
        digest[4 * index + 3] = (uint8_t)sha->state[index];
    }
}


static int nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}


int sha256_from_hex(const char *text, uint8_t digest[SHA256_BYTES])
{
    unsigned index;

    if (text == NULL || strlen(text) != 2U * SHA256_BYTES)
    {
        return 0;
    }
    for (index = 0; index < SHA256_BYTES; index++)
    {
        int high = nibble(text[2 * index]);
        int low = nibble(text[2 * index + 1]);

        if (high < 0 || low < 0)
        {
            return 0;
        }
        digest[index] = (uint8_t)(high << 4 | low);
    }
    return 1;
}
