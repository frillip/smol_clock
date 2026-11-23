/*
 * The calendar and leap second arithmetic in platform/utc.c: every day from
 * 1970-01-01 to 2106-02-07 against the C library, and the leap second rules.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../platform/utc.h"

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

static void every_day(void)
{
    unsigned days;
    int wrong = 0;

    for (days = 0; days <= UTC_LAST_DAY + 1U; days++)
    {
        time_t t = (time_t)days * 86400 + 86399;
        struct tm tm;
        UtcCivil civil;
        uint32_t seconds = (uint32_t)days * 86400U + 86399U;

        if (days == UTC_LAST_DAY + 1U)
        {
            seconds = UINT32_MAX; // 2106-02-07T06:28:15, the very last second
            t = (time_t)UINT32_MAX;
        }
        gmtime_r(&t, &tm);
        utc_to_civil(seconds, false, &civil);
        if (civil.year != tm.tm_year + 1900 || civil.month != tm.tm_mon + 1 ||
            civil.day != tm.tm_mday || civil.hour != tm.tm_hour || civil.minute != tm.tm_min ||
            civil.second != tm.tm_sec || !utc_civil_is_valid(&civil) ||
            utc_days_from_civil(civil.year, civil.month, civil.day) != days ||
            utc_from_civil(&civil) != seconds)
        {
            if (wrong++ < 5)
                printf("  day %u: %04u-%02u-%02u %02u:%02u:%02u\n", days, civil.year, civil.month,
                       civil.day, civil.hour, civil.minute, civil.second);
        }
    }
    printf("every day 1970-2106: %d wrong\n", wrong);
    CHECK(wrong == 0);
}


static void validity(void)
{
    UtcCivil c = {2106, 2, 7, 6, 28, 15};
    CHECK(utc_civil_is_valid(&c) && utc_from_civil(&c) == UINT32_MAX);
    c.second = 16;
    CHECK(!utc_civil_is_valid(&c));
    c = (UtcCivil){2106, 3, 1, 0, 0, 0};
    CHECK(!utc_civil_is_valid(&c));
    c = (UtcCivil){1969, 12, 31, 23, 59, 59};
    CHECK(!utc_civil_is_valid(&c));
    c = (UtcCivil){2024, 2, 29, 0, 0, 0};
    CHECK(utc_civil_is_valid(&c));
    c = (UtcCivil){2100, 2, 29, 0, 0, 0};
    CHECK(!utc_civil_is_valid(&c));
    c = (UtcCivil){2026, 12, 31, 23, 59, 60};
    CHECK(utc_civil_is_valid(&c) && utc_from_civil(&c) == 1798761599U);
    c = (UtcCivil){2026, 4, 31, 0, 0, 0};
    CHECK(!utc_civil_is_valid(&c));
}


static void leap_seconds(void)
{
    const uint32_t midnight = 1798761600U; // 2027-01-01
    UtcLeap insert = {midnight, 1};
    UtcLeap remove = {midnight, -1};
    bool leap;
    uint32_t t;
    UtcCivil civil;

    t = utc_advance(midnight - 2U, false, 1U, &insert, &leap);
    CHECK(t == midnight - 1U && !leap);
    t = utc_advance(midnight - 1U, false, 1U, &insert, &leap);
    CHECK(t == midnight - 1U && leap);
    utc_to_civil(t, leap, &civil);
    CHECK(civil.hour == 23 && civil.minute == 59 && civil.second == 60);
    t = utc_advance(midnight - 1U, true, 1U, &insert, &leap);
    CHECK(t == midnight && !leap);
    t = utc_advance(midnight - 10U, false, 20U, &insert, &leap);
    CHECK(t == midnight + 9U && !leap);
    t = utc_advance(midnight - 2U, false, 1U, &remove, &leap);
    CHECK(t == midnight && !leap);
    t = utc_advance(midnight - 2U, false, 1U, NULL, &leap);
    CHECK(t == midnight - 1U);

    CHECK(utc_leap_snap(midnight + 2U, 2U, &t) && t == midnight);
    CHECK(utc_leap_snap(midnight - 2U, 2U, &t) && t == midnight);
    CHECK(!utc_leap_snap(midnight + 3U, 2U, &t));
    CHECK(!utc_leap_snap(midnight - 43200U, 2U, &t));
    CHECK(utc_leap_snap(UTC_LAST_DAY * 86400U + 86399U, 2U, &t) &&
          t == (UTC_LAST_DAY + 1U) * 86400U);
    CHECK(!utc_leap_snap(UINT32_MAX, 2U, &t));
}


// utc_format_ns as it was, all 64-bit: the 32-bit one must print the same.
static void reference_format_ns(char *text, int64_t ns, bool plus)
{
    static const char *const units[] = {"ns", "us", "ms", "s"};
    uint64_t size = (uint64_t)(ns < 0 ? -ns : ns);
    const char *sign = ns < 0 ? "-" : (plus ? "+" : "");
    uint8_t unit = 0;
    uint64_t scale = 1;
    uint8_t decimals;
    uint64_t power;
    uint64_t rounded;
    uint64_t whole;

    while (unit < 3U && size >= scale * 1000U)
    {
        scale *= 1000U;
        unit++;
    }
    if (unit == 0U)
    {
        sprintf(text, "%s%lu ns", sign, (unsigned long)size);
        return;
    }
    // Four figures: as many decimals as the whole part leaves; rounding may
    // carry into another digit (9.9996 ms is 10.00 ms) or the next unit
    // (999.96 us is 1.000 ms).
    whole = size / scale;
    decimals = whole >= 1000U ? 0U : whole >= 100U ? 1U : whole >= 10U ? 2U : 3U;
    for (;;)
    {
        uint8_t digits;

        power = decimals == 3U ? 1000U : decimals == 2U ? 100U : decimals == 1U ? 10U : 1U;
        rounded = (size * power + scale / 2U) / scale;
        whole = rounded / power;
        if (whole >= 1000U && unit < 3U)
        {
            scale *= 1000U;
            unit++;
            decimals = 3U;
            continue;
        }
        digits = whole >= 100U ? 3U : whole >= 10U ? 2U : 1U;
        if (decimals > 0U && digits + decimals > 4U)
        {
            decimals--;
            continue;
        }
        break;
    }
    if (decimals == 0U)
    {
        sprintf(text, "%s%lu %s", sign, (unsigned long)whole, units[unit]);
    }
    else
    {
        sprintf(text, "%s%lu.%0*lu %s", sign, (unsigned long)whole, (int)decimals,
                (unsigned long)(rounded % power), units[unit]);
    }
}


static void nanoseconds(void)
{
    static const int64_t edges[] = {0, 1, 9, 999, 1000, 1004, 1005, 9999, 9995, 99995, 99996,
                                    999949, 999950, 999999, 1000000, 9999500, 9999949,
                                    999999499, 999999500, 1000000000, 4294967295LL,
                                    4294967296LL, 9999999999LL, 86400000000000LL,
                                    9223372036854775807LL};
    char ours[48], theirs[48];
    uint64_t seed = 12345;
    int mismatches = 0;

    printf("utc_format_ns in 32 bits: the same text as in 64\n");
    for (unsigned index = 0; index < sizeof(edges) / sizeof(edges[0]); index++)
    {
        for (int sign = -1; sign <= 1; sign += 2)
        {
            int64_t value = sign < 0 && edges[index] == 0 ? 0 : sign * edges[index];

            utc_format_ns(ours, value, true);
            reference_format_ns(theirs, value, true);
            mismatches += strcmp(ours, theirs) != 0;
            utc_format_ns(ours, value, false);
            reference_format_ns(theirs, value, false);
            mismatches += strcmp(ours, theirs) != 0;
        }
    }
    // Every scale, at random.
    for (int round = 0; round < 200000; round++)
    {
        int64_t value;

        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        value = (int64_t)((seed >> 11) >> (seed % 50U)) * ((seed & 1U) != 0U ? -1 : 1);
        utc_format_ns(ours, value, (seed & 2U) != 0U);
        reference_format_ns(theirs, value, (seed & 2U) != 0U);
        if (strcmp(ours, theirs) != 0 && mismatches++ < 5)
        {
            printf("  %lld: \"%s\" vs \"%s\"\n", (long long)value, ours, theirs);
        }
    }
    CHECK(mismatches == 0);
    utc_format_ns(ours, -9999949, false);
    CHECK(strcmp(ours, "-10.00 ms") == 0); // 9.999949 to four figures
    utc_format_ns(ours, 999999500, true);
    CHECK(strcmp(ours, "+1.000 s") == 0);
}


int main(void)
{
    every_day();
    validity();
    leap_seconds();
    nanoseconds();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
