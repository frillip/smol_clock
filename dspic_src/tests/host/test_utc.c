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


int main(void)
{
    every_day();
    validity();
    leap_seconds();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
