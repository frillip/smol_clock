// The little of the Arduino core the console commands and the PIC link use,
// for the host: streams that collect what is printed, a UART with queues.
#pragma once
#include <deque>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <strings.h>
#include <vector>

// Only named in prototypes the tests never call.
class IPAddress;

class Stream
{
  public:
    std::string out;
    virtual ~Stream()
    {
    }
    size_t printf(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char text[1024];
        va_list arguments;
        va_start(arguments, format);
        int length = vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        out += text;
        return (size_t)length;
    }
    void print(const char *text)
    {
        out += text;
    }
    void print(char c)
    {
        out += c;
    }
    void flush()
    {
    }
};

#define SERIAL_8N1 0
typedef enum
{
    UART_NO_ERROR,
    UART_BREAK_ERROR,
    UART_BUFFER_FULL_ERROR,
    UART_FIFO_OVF_ERROR,
    UART_FRAME_ERROR,
    UART_PARITY_ERROR
} hardwareSerial_error_t;

class HardwareSerial : public Stream
{
  public:
    void (*on_error)(hardwareSerial_error_t) = nullptr;
    void onReceiveError(void (*function)(hardwareSerial_error_t))
    {
        on_error = function;
    }
    std::vector<uint8_t> tx;
    std::deque<uint8_t> rx;
    HardwareSerial(int)
    {
    }
    size_t write(const uint8_t *data, size_t length)
    {
        tx.insert(tx.end(), data, data + length);
        return length;
    }
    size_t write(uint8_t byte)
    {
        tx.push_back(byte);
        return 1;
    }
    void setRxBufferSize(size_t)
    {
    }
    void setTxBufferSize(size_t)
    {
    }
    void begin(unsigned long, int, int, int)
    {
    }
    int available()
    {
        return (int)rx.size();
    }
    int read()
    {
        if (rx.empty())
            return -1;
        int byte = rx.front();
        rx.pop_front();
        return byte;
    }
};

class String
{
    std::string text;

  public:
    String(const char *value = "") : text(value)
    {
    }
    String(int value) : text(std::to_string(value))
    {
    }
    String(unsigned value) : text(std::to_string(value))
    {
    }
    const char *c_str() const
    {
        return text.c_str();
    }
};

extern HardwareSerial Serial;
extern uint32_t fake_millis;
static inline uint32_t millis(void)
{
    return fake_millis;
}
static inline void delay(unsigned long ms)
{
    fake_millis += (uint32_t)ms;
}
struct EspClass
{
    int restarts = 0;
    void restart()
    {
        restarts++;
    }
};
extern EspClass ESP;
