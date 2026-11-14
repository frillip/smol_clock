// Host stand-in for console_write(): stdout, for tests that print reports.
#include <stdio.h>

#include "../../console/console.h"

void console_write(const char *text, uint16_t length)
{
    fwrite(text, 1, length, stdout);
}
