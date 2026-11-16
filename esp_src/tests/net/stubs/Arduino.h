// Just enough of the Arduino core for net_connect.cpp and time_text.cpp.
#ifndef ARDUINO_H_STUB
#define ARDUINO_H_STUB

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// strlcpy, as the ESP's newlib has it: glibc only from 2.38 (Ubuntu 24.04);
// older ones (Ubuntu 22.04, a common WSL) lack it.
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
static inline size_t strlcpy(char *destination, const char *source, size_t size)
{
    size_t length = strlen(source);

    if (size != 0)
    {
        size_t copied = length < size - 1 ? length : size - 1;

        memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return length;
}
#endif
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
    String(const char *value = "") : text(value) {}
    String(const std::string &value) : text(value) {}
    const char *c_str() const { return text.c_str(); }
    String operator+(const char *more) const { return String(text + more); }
    String operator+(const String &more) const { return String(text + more.text); }
    friend String operator+(const char *a, const String &b) { return String(std::string(a) + b.text); }
    bool operator==(const char *other) const { return text == other; }
};

extern uint32_t fake_millis;
static inline uint32_t millis(void) { return fake_millis; }

enum IPType { IPv4, IPv6 };

// IPv4 as a.b.c.d, IPv6 as any text with a colon: what the tests use.
class IPAddress
{
    IPType kind = IPv4;
    std::string text = "0.0.0.0";

  public:
    IPAddress() {}
    IPAddress(uint32_t network_order) // as lwIP's s_addr
    {
        char t[16];
        const uint8_t *b = (const uint8_t *)&network_order;
        snprintf(t, sizeof t, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        text = t;
    }
    IPAddress(IPType type, const uint8_t *bytes, uint8_t zone = 0)
    {
        (void)zone;
        kind = type;
        text = (const char *)bytes; // the fake resolver stores the text
    }
    bool fromString(const char *value)
    {
        unsigned a, b, c, d;
        if (sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        {
            kind = IPv4;
            text = value;
            return true;
        }
        if (strchr(value, ':') != NULL && strchr(value, '[') == NULL)
        {
            kind = IPv6;
            text = value;
            return true;
        }
        return false;
    }
    IPType type() const { return kind; }
    String toString() const { return String(text); }
    bool operator==(const IPAddress &other) const { return kind == other.kind && text == other.text; }
};

#endif
