#include "debug.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "text.h"

static const char *const names[] = {"TIMING", "GNSS",   "SYNC", "SOURCES", "NTP",
                                    "MINUTE", "TUNING", "RTC",  "EVENTS"};

static uint16_t mask = DEBUG_DEFAULT;

void debug_init(void)
{
    mask = DEBUG_DEFAULT;
}


bool debug_enabled(uint16_t subsystems)
{
    return (mask & subsystems) != 0U;
}


uint16_t debug_mask(void)
{
    return mask;
}


void debug_set(uint16_t new_mask)
{
    mask = new_mask & DEBUG_ALL;
}


// The names of the bits in a mask, space-separated, or "none".
static void add_names(TextLine *line, uint16_t bits)
{
    uint8_t bit;
    bool first = true;

    for (bit = 0; bit < sizeof(names) / sizeof(names[0]); bit++)
    {
        if ((bits & (1U << bit)) != 0U)
        {
            if (!first)
            {
                text_char(line, ' ');
            }
            text_str(line, names[bit]);
            first = false;
        }
    }
    if (first)
    {
        text_str(line, "NONE");
    }
}


// With printf, so it keeps its place among other printf output.
void debug_print_names(uint16_t bits)
{
    uint8_t bit;
    bool first = true;

    for (bit = 0; bit < sizeof(names) / sizeof(names[0]); bit++)
    {
        if ((bits & (1U << bit)) != 0U)
        {
            printf("%s%s", first ? "" : " ", names[bit]);
            first = false;
        }
    }
    if (first)
    {
        printf("NONE");
    }
}


void debug_print(void)
{
    TextLine line;

    text_start(&line);
    text_str(&line, "PIC debug: ");
    add_names(&line, mask);
    text_str(&line, "\r\n");
    text_send(&line);
}
