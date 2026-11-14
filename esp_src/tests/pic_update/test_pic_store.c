/*
 * The PIC files staged on the ESP: SHA-256 against the standard vectors, the
 * file headers against make_image.py's layout, and the picfw slots against a
 * NOR flash model (a write only clears bits, an erase sets a sector): which
 * slot a file goes to, the header written last, every failure leaving the
 * slot invalid, and leftovers never trusted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/pic/pic_file.h"
#include "../../src/pic/pic_store.h"
#include "../../src/pic/sml/boot_crc.h"
#include "../../src/util/sha256.h"

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

// ---- NOR flash ------------------------------------------------------------------

static uint8_t flash[PIC_STORE_BYTES];
static long corrupt_write_at = -1; // a byte offset whose write lands wrong
static int fail_writes;            // the next writes that fail
static long erases;
static long overwrites; // a write clearing what was not erased: still NOR, counted

static bool read_flash(uint32_t offset, void *data, uint32_t length)
{
    if (offset + length > sizeof(flash))
    {
        return false;
    }
    memcpy(data, flash + offset, length);
    return true;
}


static bool write_flash(uint32_t offset, const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t index;

    if (offset + length > sizeof(flash) || fail_writes > 0)
    {
        fail_writes -= fail_writes > 0;
        return false;
    }
    for (index = 0; index < length; index++)
    {
        uint8_t value = bytes[index];

        if ((long)(offset + index) == corrupt_write_at)
        {
            value ^= 0x10;
        }
        if ((flash[offset + index] & value) != value)
        {
            overwrites++;
        }
        flash[offset + index] &= value;
    }
    return true;
}


static bool erase_flash(uint32_t offset, uint32_t length)
{
    if (offset % PIC_STORE_SECTOR != 0 || length % PIC_STORE_SECTOR != 0 ||
        offset + length > sizeof(flash))
    {
        return false;
    }
    memset(flash + offset, 0xFF, length);
    erases++;
    return true;
}


static const PicStoreFlash nor = {read_flash, write_flash, erase_flash};

// ---- Files --------------------------------------------------------------------------

static uint8_t file[0x40000];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}


static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}


// A file as make_image.py writes it. Returns its size.
static uint32_t make_file(PicFileKind kind, uint32_t version, uint32_t length, uint32_t seed)
{
    uint8_t *h = file;
    uint8_t *image = file + PIC_FILE_HEADER_BYTES;
    uint32_t index;

    srand(seed);
    for (index = 0; index < 3 * length; index++)
    {
        image[index] = (uint8_t)rand();
    }
    memset(h, 0, PIC_FILE_HEADER_BYTES);
    memcpy(h, kind == PIC_FILE_APP ? "SMOLPIC" : "SMOLSPL", 8);
    put16(h + 8, 1);
    put16(h + 10, 64);
    put32(h + 12, 0x1F6C);
    if (kind == PIC_FILE_APP)
    {
        put32(h + 16, 0x002000);
        put32(h + 20, length);
        put32(h + 24, version);
        put32(h + 28, boot_crc32(image, 3 * length));
        put32(h + 32, 1793000000);
        put16(h + 36, 0x0205);
    }
    else
    {
        put32(h + 16, version);
        put32(h + 20, 0x000100);
        put16(h + 24, 1);
        put32(h + 28, 1793000000);
        put32(h + 32, length);
        put32(h + 36, boot_crc32(image, 3 * length));
    }
    put32(h + 60, boot_crc32(h, 60));
    return PIC_FILE_HEADER_BYTES + 3 * length;
}


static void digest_of(uint32_t bytes, uint8_t digest[SHA256_BYTES])
{
    Sha256 sha;

    sha256_start(&sha);
    sha256_update(&sha, file, bytes);
    sha256_finish(&sha, digest);
}


// The file in pieces of odd sizes, as a download gives them.
static PicStoreResult store(PicFileKind kind, uint8_t slot, uint32_t bytes,
                            const uint8_t digest[SHA256_BYTES], PicStoredFile *out)
{
    PicStoreWriter writer;
    PicStoreResult result = pic_store_begin(&writer, kind, slot, bytes);
    uint32_t done = 0;

    while (result == PIC_STORE_OK && done < bytes)
    {
        uint32_t take = 1 + (uint32_t)rand() % 700;

        if (take > bytes - done)
        {
            take = bytes - done;
        }
        result = pic_store_write(&writer, file + done, take);
        done += take;
    }
    return result == PIC_STORE_OK ? pic_store_finish(&writer, digest, out) : result;
}

// ---- Tests ----------------------------------------------------------------------------

static void hex(const uint8_t *digest, char *text)
{
    int index;

    for (index = 0; index < SHA256_BYTES; index++)
    {
        sprintf(text + 2 * index, "%02x", digest[index]);
    }
}


static void test_sha256(void)
{
    static const struct
    {
        const char *text;
        const char *digest;
    } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnop"
         "qrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    uint8_t digest[SHA256_BYTES];
    uint8_t parsed[SHA256_BYTES];
    char text[2 * SHA256_BYTES + 1];
    Sha256 sha;
    unsigned index;
    unsigned split;

    printf("SHA-256\n");
    for (index = 0; index < sizeof(vectors) / sizeof(vectors[0]); index++)
    {
        size_t length = strlen(vectors[index].text);

        for (split = 0; split <= length; split += length > 60 ? 7 : 1)
        {
            sha256_start(&sha);
            sha256_update(&sha, vectors[index].text, split);
            sha256_update(&sha, vectors[index].text + split, length - split);
            sha256_finish(&sha, digest);
            hex(digest, text);
            CHECK(strcmp(text, vectors[index].digest) == 0);
        }
        CHECK(sha256_from_hex(vectors[index].digest, parsed) && memcmp(parsed, digest, 32) == 0);
    }
    sha256_start(&sha);
    for (index = 0; index < 1000; index++)
    {
        static char a[1000];

        memset(a, 'a', sizeof(a));
        sha256_update(&sha, a, sizeof(a));
    }
    sha256_finish(&sha, digest);
    hex(digest, text);
    CHECK(strcmp(text, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    CHECK(!sha256_from_hex("abc", parsed));
    CHECK(!sha256_from_hex("zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                           parsed));
    CHECK(sha256_from_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
                          parsed));
}


static void test_file(void)
{
    PicFileHeader h;
    uint32_t version;

    printf("file headers and versions\n");
    make_file(PIC_FILE_APP, 0x000400, 58838, 1);
    CHECK(pic_file_parse(file, &h) && h.kind == PIC_FILE_APP && h.length == 58838 &&
          h.version == 0x000400 && h.start == 0x2000 && h.sml_version == 0x0205 &&
          h.device_id == 0x1F6C && pic_file_bytes(&h) == 64 + 3 * 58838);
    make_file(PIC_FILE_SPL, 0x000102, 2612, 2);
    CHECK(pic_file_parse(file, &h) && h.kind == PIC_FILE_SPL && h.length == 2612 &&
          h.version == 0x000102 && h.min_ipl == 0x000100 && h.protocol == 1 && h.start == 0x800);
    file[40] ^= 1; // reserved, under the header CRC
    CHECK(!pic_file_parse(file, &h));
    make_file(PIC_FILE_SPL, 1, 3010, 2); // longer than the SPL's space
    CHECK(!pic_file_parse(file, &h));
    make_file(PIC_FILE_APP, 1, 101, 2); // odd
    CHECK(!pic_file_parse(file, &h));
    CHECK(pic_file_version("0.4.1", &version) && version == 0x000401);
    CHECK(pic_file_version("255.0.10", &version) && version == 0xFF000A);
    CHECK(!pic_file_version("0.4", &version) && !pic_file_version("0.4.1.2", &version) &&
          !pic_file_version("1.256.0", &version) && !pic_file_version("1..2", &version) &&
          !pic_file_version("v1.2.3", &version) && !pic_file_version("", &version));
}


static void test_layout(void)
{
    printf("slot layout\n");
    CHECK(pic_store_slot_offset(PIC_FILE_APP, 0) == 0x00000 &&
          pic_store_slot_offset(PIC_FILE_APP, 1) == 0x3C000 &&
          pic_store_slot_offset(PIC_FILE_SPL, 0) == 0x78000 &&
          pic_store_slot_offset(PIC_FILE_SPL, 1) == 0x7B000);
    CHECK(pic_store_slot_offset(PIC_FILE_SPL, 1) + PIC_STORE_SPL_SLOT <= PIC_STORE_BYTES);
    CHECK(PIC_STORE_APP_SLOT % PIC_STORE_SECTOR == 0 && PIC_STORE_SPL_SLOT % PIC_STORE_SECTOR == 0);
    // The largest files fit: 79,872 and 3008 instructions.
    CHECK(64 + 3 * 79872 <= PIC_STORE_APP_SLOT && 64 + 3 * 3008 <= PIC_STORE_SPL_SLOT);
}


static void test_slots(void)
{
    uint8_t digest[SHA256_BYTES];
    uint8_t other[SHA256_BYTES];
    uint8_t image[192];
    PicStoredFile got;
    uint32_t bytes;
    unsigned index;

    printf("slots\n");
    pic_store_init(NULL);
    CHECK(!pic_store_present());
    bytes = make_file(PIC_FILE_APP, 0x000301, 4000, 3);
    digest_of(bytes, digest);
    CHECK(store(PIC_FILE_APP, 0, bytes, digest, &got) == PIC_STORE_NO_PARTITION);

    // Leftovers from the old partition table: random bytes everywhere.
    srand(99);
    for (index = 0; index < sizeof(flash); index++)
    {
        flash[index] = (uint8_t)rand();
    }
    pic_store_init(&nor);
    CHECK(pic_store_present());
    CHECK(!pic_store_get(PIC_FILE_APP, 0, &got) && !pic_store_get(PIC_FILE_APP, 1, &got) &&
          !pic_store_get(PIC_FILE_SPL, 0, &got) && !pic_store_get(PIC_FILE_SPL, 1, &got));
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000301) == 0);
    CHECK(!pic_store_pick(PIC_FILE_APP, 0x000301, &got) && !got.valid);

    // 0.3.1 into slot 0.
    CHECK(store(PIC_FILE_APP, 0, bytes, digest, &got) == PIC_STORE_OK);
    CHECK(got.valid && got.slot == 0 && got.header.version == 0x000301 && overwrites == 0);
    CHECK(pic_store_read_image(&got, 3 * 64, image, sizeof(image)) &&
          memcmp(image, file + 64 + 3 * 64, sizeof(image)) == 0);
    CHECK(!pic_store_read_image(&got, 3 * 4000 - 3, image, 6));
    CHECK(pic_store_find(PIC_FILE_APP, 0x000301, digest, &got) && got.slot == 0);
    memset(other, 0, sizeof(other));
    CHECK(!pic_store_find(PIC_FILE_APP, 0x000301, other, &got));
    CHECK(!pic_store_find(PIC_FILE_APP, 0x000302, digest, &got));

    // The invalid slot first; then the one the PIC is not running.
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000301) == 1);
    bytes = make_file(PIC_FILE_APP, 0x000400, 5000, 4);
    digest_of(bytes, digest);
    CHECK(store(PIC_FILE_APP, 1, bytes, digest, &got) == PIC_STORE_OK && got.slot == 1);
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000301) == 1); // 0.3.1 runs: 0.4.0's slot goes
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000400) == 0); // 0.4.0 runs: the last good stays
    CHECK(pic_store_choose(PIC_FILE_APP, 0) == 0);        // unknown: the lower version goes
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000500) == 0);

    // Without the network: the one the PIC runs, else the newest.
    CHECK(pic_store_pick(PIC_FILE_APP, 0x000301, &got) && got.slot == 0);
    CHECK(pic_store_pick(PIC_FILE_APP, 0x000400, &got) && got.slot == 1);
    CHECK(pic_store_pick(PIC_FILE_APP, 0, &got) && got.slot == 1);
    CHECK(pic_store_pick(PIC_FILE_APP, 0x000302, &got) && got.slot == 1);
    CHECK(!pic_store_pick(PIC_FILE_SPL, 0, &got));

    // Emptied: the header's sector only, the other slot untouched.
    CHECK(pic_store_erase(PIC_FILE_APP, 1) && !pic_store_get(PIC_FILE_APP, 1, &got));
    CHECK(pic_store_get(PIC_FILE_APP, 0, &got) && got.header.version == 0x000301);
    CHECK(!pic_store_erase(PIC_FILE_APP, PIC_STORE_SLOTS));
    CHECK(pic_store_pick(PIC_FILE_APP, 0x000400, &got) && got.slot == 0);
    CHECK(pic_store_choose(PIC_FILE_APP, 0x000301) == 1);
    CHECK(store(PIC_FILE_APP, 1, bytes, digest, &got) == PIC_STORE_OK);

    // Failures leave the slot invalid.
    bytes = make_file(PIC_FILE_APP, 0x000500, 3000, 5);
    digest_of(bytes, digest);
    memcpy(other, digest, sizeof(other));
    other[5] ^= 1;
    CHECK(store(PIC_FILE_APP, 0, bytes, other, &got) == PIC_STORE_SHA256);
    CHECK(!pic_store_get(PIC_FILE_APP, 0, &got));
    {
        PicStoreWriter writer;

        CHECK(pic_store_begin(&writer, PIC_FILE_APP, 0, bytes) == PIC_STORE_OK);
        CHECK(pic_store_write(&writer, file, bytes - 10) == PIC_STORE_OK);
        CHECK(pic_store_finish(&writer, digest, &got) == PIC_STORE_SHORT);
        CHECK(!pic_store_get(PIC_FILE_APP, 0, &got));
        CHECK(pic_store_begin(&writer, PIC_FILE_APP, 0, bytes) == PIC_STORE_OK);
        CHECK(pic_store_write(&writer, file, bytes) == PIC_STORE_OK);
        CHECK(pic_store_write(&writer, file, 1) == PIC_STORE_TOO_BIG);
        // A power cut before the finish: the header was never written.
        CHECK(pic_store_begin(&writer, PIC_FILE_APP, 0, bytes) == PIC_STORE_OK);
        CHECK(pic_store_write(&writer, file, bytes) == PIC_STORE_OK);
        CHECK(!pic_store_get(PIC_FILE_APP, 0, &got));
    }
    corrupt_write_at = (long)pic_store_slot_offset(PIC_FILE_APP, 0) + 1000;
    CHECK(store(PIC_FILE_APP, 0, bytes, digest, &got) == PIC_STORE_IMAGE_CRC);
    corrupt_write_at = -1;
    CHECK(!pic_store_get(PIC_FILE_APP, 0, &got));
    fail_writes = 1;
    CHECK(store(PIC_FILE_APP, 0, bytes, digest, &got) == PIC_STORE_FLASH);
    CHECK(!pic_store_get(PIC_FILE_APP, 0, &got));
    CHECK(store(PIC_FILE_APP, 0, PIC_STORE_APP_SLOT + 1, digest, &got) == PIC_STORE_TOO_BIG);
    // An SPL file where an application goes, and the other way.
    bytes = make_file(PIC_FILE_SPL, 0x000100, 2612, 6);
    digest_of(bytes, digest);
    CHECK(store(PIC_FILE_APP, 0, bytes, digest, &got) == PIC_STORE_HEADER);
    CHECK(store(PIC_FILE_SPL, 0, bytes, digest, &got) == PIC_STORE_OK && got.valid);
    bytes = make_file(PIC_FILE_APP, 0x000500, 100, 6);
    digest_of(bytes, digest);
    CHECK(store(PIC_FILE_SPL, 1, bytes, digest, &got) == PIC_STORE_HEADER);
    CHECK(!pic_store_get(PIC_FILE_SPL, 1, &got));
    // The SPL write left the application slots alone.
    CHECK(pic_store_get(PIC_FILE_APP, 1, &got) && got.header.version == 0x000400);
    // Writing slot 0 again left slot 1 alone, and nothing was written over.
    CHECK(overwrites == 0);
}


int main(void)
{
    test_sha256();
    test_file();
    test_layout();
    test_slots();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
