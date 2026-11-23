// platform/format.c against the C library's snprintf: every conversion and
// flag the firmware uses, and the edges.
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../platform/format.h"

static int failures, checks;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        checks++;                                                            \
        if (!(cond))                                                         \
        {                                                                    \
            failures++;                                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                 \
        }                                                                    \
    } while (0)

// Both formatters, one format and its arguments: the same text and length.
#define SAME(...)                                                                      \
    do                                                                                 \
    {                                                                                  \
        char ours[160], theirs[160];                                                   \
        int our_length = format_snprintf(ours, sizeof(ours), __VA_ARGS__);             \
        int their_length = snprintf(theirs, sizeof(theirs), __VA_ARGS__);              \
        checks++;                                                                      \
        if (our_length != their_length || strcmp(ours, theirs) != 0)                   \
        {                                                                              \
            failures++;                                                                \
            printf("  FAIL line %d: \"%s\" (%d) vs \"%s\" (%d)\n", __LINE__, ours,     \
                   our_length, theirs, their_length);                                  \
        }                                                                              \
    } while (0)

int main(void)
{
    static const long longs[] = {0, 1, -1, 9, 10, 99, 100, 12345, -12345, 65535, 65536,
                                 2147483647L, -2147483647L - 1L, 1000000000L, 999999999L};
    static const unsigned long ulongs[] = {0, 1, 9, 10, 65535, 65536, 655359, 655360,
                                           4294967295UL, 4000000000UL, 1795000000UL};
    static const int ints[] = {0, 1, -1, 7, 42, -42, 32767, -32768};
    char text[64];
    unsigned index;

    printf("format: our printf family against the C library's\n");
    for (index = 0; index < sizeof(longs) / sizeof(longs[0]); index++)
    {
        long value = longs[index];

        SAME("%ld", value);
        SAME("%+ld", value);
        SAME("% ld", value);
        SAME("%4ld", value);
        SAME("%+9ld", value);
        SAME("%-8ld|", value);
        SAME("%08ld", value);
        SAME("%+08ld", value);
        SAME("%.5ld", value);
        SAME("%8.5ld", value);
        SAME("%-+8.3ld|", value);
    }
    for (index = 0; index < sizeof(ulongs) / sizeof(ulongs[0]); index++)
    {
        unsigned long value = ulongs[index];

        SAME("%lu", value);
        SAME("%02lu", value);
        SAME("%03lu", value);
        SAME("%06lu", value);
        SAME("%07lu", value);
        SAME("%10lu", value);
        SAME("%-10lu|", value);
        SAME("%lX %lx", value, value);
        SAME("%05lX", value);
        SAME("%06lX", value);
        SAME("%#lx %#lX", value, value);
        SAME("%0*lu", 5, value);
        SAME("%*lu|%-*lu|", 7, value, 7, value);
        SAME("%.0lu|", value);
    }
    for (index = 0; index < sizeof(ints) / sizeof(ints[0]); index++)
    {
        int value = ints[index];

        SAME("%d", value);
        SAME("%+d", value);
        SAME("%05d", value);
        SAME("%u", (unsigned)(value & 0xFFFF));
        SAME("%02u %03u %04u %4u %5u", (unsigned)(value & 0xFF), (unsigned)(value & 0xFF),
             (unsigned)(value & 0xFFF), (unsigned)(value & 0xFFF), (unsigned)(value & 0xFFFF));
        SAME("%02X %04X", (unsigned)(value & 0xFF), (unsigned)(value & 0xFFFF));
        SAME("%hu %hhu", (unsigned short)value, (unsigned char)value);
    }
    SAME("%s", "text");
    SAME("%-5s|%-14s|%-20s|", "ab", "abcdefghijklmnopq", "");
    SAME("%8s|%.3s|%8.3s|%-8.3s|", "abcdef", "abcdef", "abcdef", "abcdef");
    SAME("%.*s|", 2, "abcdef");
    SAME("%c%c%c", 'a', '+', ' ');
    SAME("%3c|%-3c|", 'x', 'y');
    SAME("100%% sure, %d%%", 5);
    SAME("%04u-%02u-%02uT%02u:%02u:%02uZ", 2026U, 11U, 23U, 7U, 4U, 59U);
    SAME("%c%02lu:%02lu", '-', 5UL, 30UL);
    SAME("%s%lu.%0*lu %s", "-", 12UL, 3, 45UL, "ms");
    SAME("%s", "");
    SAME("plain");
    SAME("%lld %llu %+lld", (long long)-1234567890123LL, 18446744073709551615ULL,
         9000000000LL);
    SAME("%llX", 0x123456789ABCULL);

    printf("  cut short, and the lengths\n");
    CHECK(format_snprintf(text, 5, "%lu", 1234567UL) == 7 && strcmp(text, "1234") == 0);
    CHECK(format_snprintf(text, 1, "%s", "abc") == 3 && text[0] == '\0');
    CHECK(format_snprintf(NULL, 0, "%d", 12345) == 5);
    CHECK(format_sprintf(text, "%s-%u", "a", 7U) == 3 && strcmp(text, "a-7") == 0);

    printf("  not supported: shown as written\n");
    {
        const char *unsupported = "a %f b"; // not a literal: no compile-time check

        CHECK(format_snprintf(text, sizeof(text), unsupported) == 6 &&
              strcmp(text, "a %f b") == 0);
    }

    printf("  printf: a line at a time\n");
    CHECK(format_printf("  printed by format_printf: %ld, %s\n", -5L, "ok") == 35);
    format_printf("  (a partial line, ");
    format_printf("then the rest)\n");
    format_flush();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
