#include "console.h"

#include <stdarg.h>

#include "../net/telnet.h"

void console_line_clear(ConsoleLine *line)
{
    line->length = 0;
    line->text[0] = '\0';
}


bool console_read_line(ConsoleLine *line, Stream *input, bool echo)
{
    while (input->available())
    {
        char c = (char)input->read();
        char previous = line->previous;

        line->previous = c;
        if (c == '\r' || c == '\n')
        {
            if (c == '\n' && previous == '\r')
            {
                continue; // the LF of a CR LF
            }
            if (echo)
            {
                input->print("\r\n");
            }
            line->text[line->length] = '\0';
            return true;
        }
        if (c == 0x7F || c == '\b')
        {
            if (line->length > 0)
            {
                line->length--;
                if (echo)
                {
                    input->print("\b \b");
                }
            }
            continue;
        }
        if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x7F)
        {
            continue; // other control characters, telnet negotiation, non-ASCII
        }
        if (line->length >= CONSOLE_LINE_BYTES - 1)
        {
            input->print("\r\nLine too long\r\n> ");
            console_line_clear(line);
            continue;
        }
        line->text[line->length++] = c;
        if (echo)
        {
            input->print(c);
        }
    }
    return false;
}


void console_write_text(const uint8_t *text, size_t length)
{
    Serial.write(text, length);
    telnet_write_text(text, length);
}


namespace
{
class ConsoleStream : public Stream
{
  public:
    size_t write(uint8_t byte) override
    {
        console_write_text(&byte, 1);
        return 1;
    }
    size_t write(const uint8_t *buffer, size_t size) override
    {
        console_write_text(buffer, size);
        return size;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

ConsoleStream console_out;
} // namespace


Stream *console_stream(void)
{
    return &console_out;
}


void console_printf(const char *format, ...)
{
    char text[256];
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (length > 0)
    {
        console_write_text((const uint8_t *)text,
                           (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
    }
}
