/*
 * ntp_select: which servers agree, which is followed, and what happens
 * without a majority.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/timing/ntp_select.h"

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

static NtpCandidate servers[NTP_SELECT_MAX];

static void set(int index, int64_t offset_us, int64_t distance_us)
{
    servers[index].configured = true;
    servers[index].usable = true;
    servers[index].offset_us = offset_us;
    servers[index].distance_us = distance_us;
}


static void clear(void)
{
    memset(servers, 0, sizeof servers);
}


// The markers, server 1 first: "*+x? ".
static const char *markers(const NtpSelection *selection)
{
    static char text[NTP_SELECT_MAX + 1];

    for (int index = 0; index < NTP_SELECT_MAX; index++)
    {
        text[index] = ntp_verdict_marker(selection->verdict[index], true);
    }
    text[NTP_SELECT_MAX] = '\0';
    return text;
}


int main(void)
{
    NtpSelection selection;

    printf("None and one\n");
    clear();
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == -1 && selection.usable == 0 &&
          strcmp(markers(&selection), "     ") == 0);
    servers[0].configured = true; // configured, nothing heard yet
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == -1 && strcmp(markers(&selection), "?    ") == 0);
    set(0, 500, 5000);
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == 0 && selection.majority &&
          strcmp(markers(&selection), "*    ") == 0);

    printf("Agreeing: the closest, kept unless another is twice as close\n");
    set(1, -1000, 3000);
    set(2, 2000, 4000);
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == 1 && selection.agreeing == 3 &&
          strcmp(markers(&selection), "+*+  ") == 0);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection); // 3000 is not half of 5000
    CHECK(selection.selected == 0 && strcmp(markers(&selection), "*++  ") == 0);
    servers[1].distance_us = 2400;
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    CHECK(selection.selected == 1);

    printf("Young lines: chosen by rank, the distance itself unchanged\n");
    NtpCandidate kept[NTP_SELECT_MAX];
    memcpy(kept, servers, sizeof kept);
    clear();
    set(0, 0, 30000);   // LAN, young: 1.5 ms but for its skew x age
    set(1, 1300, 9000); // Internet, polled last
    servers[0].rank_us = 1500;
    servers[1].rank_us = 9000;
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == 0 && selection.agreeing == 2);
    // Kept against the other's rank, not its distance.
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    CHECK(selection.selected == 0);
    servers[1].rank_us = 700;
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    CHECK(selection.selected == 1);
    // No rank: the distance, as before.
    servers[0].rank_us = 0;
    servers[1].rank_us = 0;
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.selected == 1);
    memcpy(servers, kept, sizeof kept);

    printf("A falseticker outvoted\n");
    clear();
    set(0, 40000, 2000); // 40 ms out
    set(1, 100, 3000);
    set(2, -200, 3000);
    set(3, 900, 6000);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    CHECK(selection.majority && selection.agreeing == 3 && selection.selected == 1);
    CHECK(strcmp(markers(&selection), "x*++ ") == 0);
    // One gone quiet: still a majority of the usable ones.
    servers[3].usable = false;
    ntp_select(servers, NTP_SELECT_MAX, 1, &selection);
    CHECK(selection.majority && selection.usable == 3 && strcmp(markers(&selection), "x*+? ") == 0);

    printf("Two that disagree: keep the one in use, else the first\n");
    clear();
    set(0, 0, 1000);
    set(1, 20000, 1000);
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(!selection.majority && selection.selected == 0 &&
          strcmp(markers(&selection), "*x   ") == 0);
    ntp_select(servers, NTP_SELECT_MAX, 1, &selection);
    CHECK(!selection.majority && selection.selected == 1 &&
          strcmp(markers(&selection), "x*   ") == 0);
    // Even when the other is closer.
    servers[0].distance_us = 300;
    ntp_select(servers, NTP_SELECT_MAX, 1, &selection);
    CHECK(selection.selected == 1);

    printf("Two pairs\n");
    clear();
    set(0, 0, 1000);
    set(1, 500, 1000);
    set(2, 30000, 1000);
    set(3, 30500, 500);
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(!selection.majority && selection.agreeing == 2 &&
          strcmp(markers(&selection), "*+xx ") == 0);
    ntp_select(servers, NTP_SELECT_MAX, 2, &selection);
    CHECK(selection.selected == 2 && strcmp(markers(&selection), "xx*+ ") == 0);

    printf("Touching intervals agree; a chain is not one set\n");
    clear();
    set(0, 0, 1000);
    set(1, 2000, 1000); // meets 0 at +1000
    set(2, 4000, 1000); // meets 1 at +3000, not 0
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    CHECK(selection.agreeing == 2 && selection.majority &&
          strcmp(markers(&selection), "*+x  ") == 0);
    ntp_select(servers, NTP_SELECT_MAX, 2, &selection);
    CHECK(strcmp(markers(&selection), "x+*  ") == 0);

    printf("Combining: weighted by 1 / distance, the far and the outvoted left out\n");
    uint8_t distant[NTP_SELECT_MAX] = {0};
    clear();
    set(0, 100, 1000);
    set(1, 400, 1000);
    set(2, -200, 1250);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    // (100/1000 + 400/1000 - 200/1250) / (1/1000 + 1/1000 + 1/1250) = 121
    CHECK(selection.selected == 0 && selection.combined == 3 &&
          selection.combined_offset_us == 121);
    set(2, -200, 1651); // beyond 1.5 times (100 us + the followed one's distance)
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(selection.combined == 2 && selection.combined_offset_us == 250 &&
          !selection.in_combined[2] && distant[2] == NTP_COMBINE_DISTANT_STEP &&
          ntp_verdict_marker(selection.verdict[2], selection.in_combined[2]) == '-');
    // Left out again and again: the count stops at the penalty.
    for (int i = 0; i < 6; i++)
        ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(distant[2] == NTP_COMBINE_DISTANT);
    // Close again: out until the count runs down, one a combining.
    set(2, -200, 1250);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    for (int i = 0; i < NTP_COMBINE_DISTANT; i++)
    {
        ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
        if (selection.in_combined[2])
            break;
    }
    CHECK(!selection.in_combined[2] && distant[2] == 0);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(selection.in_combined[2] && selection.combined_offset_us == 121);
    set(2, 50000, 1000); // a falseticker
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(selection.combined == 2 && selection.combined_offset_us == 250 && distant[2] == 0);
    // A young server: not combined, however close, and no count, so it
    // joins as soon as its line has grown.
    set(2, -200, 1651);
    servers[2].young = true;
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(!selection.in_combined[2] && distant[2] == 0);
    set(2, -200, 1250);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(!selection.in_combined[2] && distant[2] == 0);
    servers[2].young = false;
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant, &selection);
    CHECK(selection.in_combined[2]);

    printf("Combining frequencies: by 1 / sd^2, the skew by 1 / sqrt(sum 1 / skew^2)\n");
    uint8_t distant2[NTP_SELECT_MAX] = {0};
    clear();
    set(0, 100, 1000);
    set(1, 400, 1000);
    set(2, -200, 1250);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant2, &selection);
    CHECK(!selection.frequency_known); // no lines yet: offsets only
    for (int index = 0; index < 3; index++)
    {
        servers[index].fitted = true;
        servers[index].skew_ppb = 100.0;
    }
    servers[0].frequency_ppb = 1000.0, servers[0].frequency_sd_ppb = 10.0;
    servers[1].frequency_ppb = 1100.0, servers[1].frequency_sd_ppb = 20.0;
    servers[2].frequency_ppb = 1000.0, servers[2].frequency_sd_ppb = 10.0;
    servers[2].fitted = false; // its offset only
    ntp_combine(servers, NTP_SELECT_MAX, distant2, &selection);
    // (1000/100 + 1100/400) / (1/100 + 1/400) = 1020; 100 / sqrt(2)
    CHECK(selection.combined == 3 && selection.frequency_known &&
          fabs(selection.combined_frequency_ppb - 1020.0) < 1e-6 &&
          fabs(selection.combined_skew_ppb - 100.0 / sqrt(2.0)) < 1e-6);
    servers[1].frequency_ppb = 1000.0 + 1.5 * 200.0 + 1.0; // beyond 1.5 x the skews
    ntp_combine(servers, NTP_SELECT_MAX, distant2, &selection);
    CHECK(selection.combined == 2 && !selection.in_combined[1] &&
          fabs(selection.combined_frequency_ppb - 1000.0) < 1e-6);

    printf("Combining: the limit runs from the least distance, not the followed one's\n");
    {
        uint8_t distant3[NTP_SELECT_MAX] = {0};

        clear();
        set(0, 100, 11300); // followed, kept though the furthest
        set(1, 400, 13800);
        set(2, -200, 8400);
        set(3, 300, 7600);
        ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
        CHECK(selection.selected == 0);
        ntp_combine(servers, NTP_SELECT_MAX, distant3, &selection);
        // 1.5 x (7600 + 100) = 11550: server 1 at 13800 out, the followed one in.
        CHECK(selection.in_combined[0] && !selection.in_combined[1] && selection.in_combined[2] &&
              selection.in_combined[3] && selection.combined == 3);
    }

    printf("Combining: without a majority, the followed server alone\n");
    clear();
    set(0, 0, 1000);
    set(1, 30000, 1000);
    ntp_select(servers, NTP_SELECT_MAX, 0, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant2, &selection);
    CHECK(!selection.majority && selection.combined == 1 && selection.combined_offset_us == 0);
    clear();
    ntp_select(servers, NTP_SELECT_MAX, -1, &selection);
    ntp_combine(servers, NTP_SELECT_MAX, distant2, &selection);
    CHECK(selection.selected < 0 && selection.combined == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
