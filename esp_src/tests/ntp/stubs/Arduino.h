// Just enough of the Arduino core for ntp.cpp and time_text.cpp.
#ifndef ARDUINO_H_STUB
#define ARDUINO_H_STUB

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

class Stream
{
  public:
    std::string out;
    size_t print(const char *text)
    {
        out += text;
        return strlen(text);
    }
    size_t print(char c)
    {
        out += c;
        return 1;
    }
    int printf(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char text[512];
        va_list args;
        va_start(args, format);
        int n = vsnprintf(text, sizeof text, format, args);
        va_end(args);
        out += text;
        return n;
    }
};

class String
{
    std::string text;

  public:
    String(const char *value = "") : text(value)
    {
    }
    const char *c_str() const
    {
        return text.c_str();
    }
};

extern uint32_t fake_millis;
static inline uint32_t millis(void)
{
    return fake_millis;
}

enum IPType
{
    IPv4,
    IPv6
};

// 192.0.2.n, or 2001:db8::n: n is all a test needs.
class IPAddress
{
    IPType kind = IPv4;
    uint8_t bytes[16] = {0};

  public:
    IPAddress()
    {
    }
    IPAddress(IPType type, uint8_t last)
    {
        kind = type;
        if (type == IPv4)
        {
            bytes[0] = 192, bytes[1] = 0, bytes[2] = 2, bytes[3] = last;
        }
        else
        {
            bytes[0] = 0x20, bytes[1] = 0x01, bytes[2] = 0x0d, bytes[3] = 0xb8, bytes[15] = last;
        }
    }
    IPType type() const
    {
        return kind;
    }
    bool operator==(const IPAddress &other) const
    {
        return kind == other.kind && memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
    }
    uint8_t zone() const
    {
        return 0;
    }
    uint8_t &operator[](int index)
    {
        return bytes[index];
    }
    String toString() const
    {
        char text[48];
        if (kind == IPv4)
            snprintf(text, sizeof text, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
        else
            snprintf(text, sizeof text, "2001:db8::%u", bytes[15]);
        return String(text);
    }
};

#endif
