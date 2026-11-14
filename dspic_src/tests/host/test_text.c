/*
 * The fast formatter (console/text.c) against printf, for every format the
 * per-epoch reports use, over edge cases and random values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../console/text.h"

static int failures, checks;
static char sent[1000];

void console_write(const char *text, uint16_t length)
{
    memcpy(sent, text, length);
    sent[length] = '\0';
}


static void same(TextLine *line, const char *expected, const char *what)
{
    checks++;
    if (line->length != strlen(expected) || memcmp(line->text, expected, line->length) != 0)
    {
        failures++;
        if (failures < 10)
            printf("  FAIL %s: got '%.*s', want '%s'\n", what, line->length, line->text, expected);
    }
}


static void u32_case(uint32_t v)
{
    static const struct
    {
        uint8_t width;
        char pad;
        const char *format;
    } formats[] = {{0, ' ', "%lu"},   {5, '0', "%05lu"}, {10, ' ', "%10lu"},
                   {3, '0', "%03lu"}, {2, '0', "%02lu"}, {4, '0', "%04lu"}};
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++)
    {
        TextLine line;
        char want[40];
        text_start(&line);
        text_u32(&line, v, formats[i].width, formats[i].pad);
        snprintf(want, sizeof want, formats[i].format, (unsigned long)v);
        same(&line, want, formats[i].format);
    }
}


static void i32_case(int32_t v)
{
    TextLine line;
    char want[40];
    text_start(&line);
    text_i32(&line, v, 4, false);
    snprintf(want, sizeof want, "%4ld", (long)v);
    same(&line, want, "%4ld");
    text_start(&line);
    text_i32(&line, v, 0, true);
    snprintf(want, sizeof want, "%+ld", (long)v);
    same(&line, want, "%+ld");
    text_start(&line);
    text_i32(&line, v, 0, false);
    snprintf(want, sizeof want, "%ld", (long)v);
    same(&line, want, "%ld");
}


int main(void)
{
    static const uint32_t edges[] = {
        0,    1,     9,          10,         99,          100,          999,         1000,
        9999, 10000, 39999924UL, 40000000UL, 999999999UL, 1000000000UL, 4294967295UL};
    TextLine line;
    char want[40];

    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++)
    {
        u32_case(edges[i]);
        i32_case((int32_t)edges[i]);
        i32_case(-(int32_t)(edges[i] & 0x7FFFFFFFUL));
    }
    i32_case(INT32_MIN);
    i32_case(INT32_MAX);
    srand(7);
    for (int k = 0; k < 100000; k++)
    {
        uint32_t v = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        u32_case(v >> (rand() % 32));
        i32_case((int32_t)(v >> (rand() % 32)) * (rand() % 2 ? 1 : -1));
    }
    for (uint32_t v = 0; v <= 0xFFFF; v += 0x1111)
    {
        text_start(&line);
        text_hex16(&line, (uint16_t)v);
        snprintf(want, sizeof want, "%04X", (unsigned)v);
        same(&line, want, "%04X");
    }
    // A long line stops at its capacity; text_send hands it over and resets.
    text_start(&line);
    for (int i = 0; i < 300; i++)
        text_char(&line, 'x');
    checks++;
    if (line.length != TEXT_LINE_CAPACITY)
        failures++;
    text_start(&line);
    text_str(&line, "abc");
    text_send(&line);
    checks++;
    if (strcmp(sent, "abc") != 0 || line.length != 0)
        failures++;

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
