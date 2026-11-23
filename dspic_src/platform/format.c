#define FORMAT_NO_MAPPING // this file's own snprintf is the library's name
#include "format.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __XC16__
int write(int handle, void *buffer, unsigned int len); // console/console.c
#define HARDWARE_DIVIDE
#elif defined(FORMAT_TEST_HARDWARE_DIVIDE)
// The host tests' stand-in for DIV.UD: 32 by 16 bits, the quotient must fit
// 16 (on the dsPIC, an overflow gives garbage).
#include <assert.h>
static uint16_t divud(uint32_t dividend, uint16_t divisor)
{
    assert(dividend / divisor <= 0xFFFFU);
    return (uint16_t)(dividend / divisor);
}


#define __builtin_divud divud
#define HARDWARE_DIVIDE
#endif

// Where formatted text goes: a string (text, size: at most size - 1 kept,
// always terminated) or, text NULL, printf's line.
typedef struct
{
    char *text;
    size_t size;
    size_t length; // as if nothing were cut
} Out;

static char line[FORMAT_LINE_BYTES];
static uint16_t line_length;

void format_flush(void)
{
    if (line_length == 0U)
    {
        return;
    }
#ifdef __XC16__
    (void)write(1, line, line_length);
#else
    (void)fwrite(line, 1, line_length, stdout);
#endif
    line_length = 0;
}


static void put(Out *out, const char *from, size_t count)
{
    size_t index;

    if (out->text != NULL)
    {
        for (index = 0; index < count; index++)
        {
            if (out->length + index + 1U < out->size)
            {
                out->text[out->length + index] = from[index];
            }
        }
        out->length += count;
        return;
    }
    for (index = 0; index < count; index++)
    {
        line[line_length++] = from[index];
        if (from[index] == '\n' || line_length == FORMAT_LINE_BYTES)
        {
            format_flush();
        }
    }
    out->length += count;
}


static void put_repeat(Out *out, char fill, int count)
{
    while (count-- > 0)
    {
        put(out, &fill, 1);
    }
}


/*
 * value / 10, and the remainder. On the dsPIC by its hardware divide, 32
 * by 16 bits with a 16-bit quotient: the high half first, then the
 * remainder with the low half (under 10 x 65536, so its quotient fits).
 */
static uint32_t divide_by_10(uint32_t value, uint8_t *remainder)
{
#ifdef HARDWARE_DIVIDE
    uint16_t high = (uint16_t)(value >> 16);
    uint16_t high_quotient = high / 10U;
    uint32_t low = ((uint32_t)(uint16_t)(high - high_quotient * 10U) << 16) | (uint16_t)value;
    uint16_t low_quotient = __builtin_divud(low, 10U);

    *remainder = (uint8_t)((uint16_t)low - (uint16_t)(low_quotient * 10U));
    return ((uint32_t)high_quotient << 16) | low_quotient;
#else
    *remainder = (uint8_t)(value % 10U);
    return value / 10U;
#endif
}


// The digits of value, backwards from end; how many. Zero is one digit.
static uint8_t digits_32(uint32_t value, uint8_t base, bool upper, char *end)
{
    const char *symbols = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint8_t count = 0;

    do
    {
        uint8_t digit;

        if (base == 16U)
        {
            digit = (uint8_t)(value & 0xFU);
            value >>= 4;
        }
        else
        {
            value = divide_by_10(value, &digit);
        }
        *--end = symbols[digit];
        count++;
    } while (value != 0U);
    return count;
}


// The same for 64 bits (ll): the library's 64-bit divide, so slow.
static uint8_t digits_64(unsigned long long value, uint8_t base, bool upper, char *end)
{
    const char *symbols = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint8_t count = 0;

    if (value <= 0xFFFFFFFFULL)
    {
        return digits_32((uint32_t)value, base, upper, end);
    }
    do
    {
        *--end = symbols[(uint8_t)(value % base)];
        value /= base;
        count++;
    } while (value != 0U);
    return count;
}


typedef struct
{
    bool left;
    bool plus;
    bool space;
    bool zero;
    bool alternate;
    int width;     // -1 none
    int precision; // -1 none
} Spec;

static void put_number(Out *out, const Spec *spec, unsigned long long magnitude, bool negative,
                       bool is_signed, uint8_t base, bool upper)
{
    char digits[24];
    char prefix[3];
    uint8_t prefix_length = 0;
    int count;
    int zeros = 0;
    int pad;

    count = digits_64(magnitude, base, upper, digits + sizeof(digits));
    if (spec->precision == 0 && magnitude == 0U)
    {
        count = 0;
    }
    if (is_signed)
    {
        if (negative)
        {
            prefix[prefix_length++] = '-';
        }
        else if (spec->plus)
        {
            prefix[prefix_length++] = '+';
        }
        else if (spec->space)
        {
            prefix[prefix_length++] = ' ';
        }
    }
    if (spec->alternate && base == 16U && magnitude != 0U)
    {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = upper ? 'X' : 'x';
    }
    if (spec->precision > count)
    {
        zeros = spec->precision - count;
    }
    pad = spec->width - (prefix_length + zeros + count);
    if (pad > 0 && spec->zero && !spec->left && spec->precision < 0)
    {
        zeros += pad;
        pad = 0;
    }
    if (!spec->left)
    {
        put_repeat(out, ' ', pad);
    }
    put(out, prefix, prefix_length);
    put_repeat(out, '0', zeros);
    put(out, digits + sizeof(digits) - count, (size_t)count);
    if (spec->left)
    {
        put_repeat(out, ' ', pad);
    }
}


static void put_padded(Out *out, const Spec *spec, const char *text, size_t length)
{
    int pad = spec->width - (int)length;

    if (!spec->left)
    {
        put_repeat(out, ' ', pad);
    }
    put(out, text, length);
    if (spec->left)
    {
        put_repeat(out, ' ', pad);
    }
}


// A number or *, from the format or the arguments.
static int read_count(const char **format, va_list *args)
{
    int count = 0;

    if (**format == '*')
    {
        (*format)++;
        return va_arg(*args, int);
    }
    while (**format >= '0' && **format <= '9')
    {
        count = count * 10 + (**format - '0');
        (*format)++;
    }
    return count;
}


static int format_out(Out *out, const char *format, va_list given)
{
    va_list args;

    va_copy(args, given);
    while (*format != '\0')
    {
        const char *start;
        const char *plain = format;
        Spec spec;
        uint8_t longs = 0;
        char conversion;

        while (*format != '\0' && *format != '%')
        {
            format++;
        }
        put(out, plain, (size_t)(format - plain));
        if (*format == '\0')
        {
            break;
        }
        start = format++;
        memset(&spec, 0, sizeof(spec));
        spec.width = -1;
        spec.precision = -1;
        for (;; format++)
        {
            if (*format == '-')
            {
                spec.left = true;
            }
            else if (*format == '+')
            {
                spec.plus = true;
            }
            else if (*format == ' ')
            {
                spec.space = true;
            }
            else if (*format == '0')
            {
                spec.zero = true;
            }
            else if (*format == '#')
            {
                spec.alternate = true;
            }
            else
            {
                break;
            }
        }
        if (*format == '*' || (*format >= '0' && *format <= '9'))
        {
            spec.width = read_count(&format, &args);
            if (spec.width < 0)
            {
                spec.left = true; // a negative * width: left-justified
                spec.width = -spec.width;
            }
        }
        if (*format == '.')
        {
            format++;
            spec.precision = read_count(&format, &args);
            if (spec.precision < 0)
            {
                spec.precision = -1; // a negative * precision: as if none
            }
        }
        while (*format == 'l' || *format == 'h')
        {
            if (*format == 'l')
            {
                longs++;
            }
            format++;
        }
        conversion = *format;
        if (conversion != '\0')
        {
            format++;
        }
        switch (conversion)
        {
            case 'd':
            case 'i':
            {
                long long value = longs >= 2U   ? va_arg(args, long long)
                                  : longs == 1U ? (long long)va_arg(args, long)
                                                : (long long)va_arg(args, int);
                bool negative = value < 0;
                unsigned long long magnitude =
                    negative ? (unsigned long long)(-(value + 1)) + 1U : (unsigned long long)value;

                put_number(out, &spec, magnitude, negative, true, 10, false);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            {
                unsigned long long value = longs >= 2U ? va_arg(args, unsigned long long)
                                           : longs == 1U
                                               ? (unsigned long long)va_arg(args, unsigned long)
                                               : (unsigned long long)va_arg(args, unsigned int);

                put_number(out, &spec, value, false, false, conversion == 'u' ? 10U : 16U,
                           conversion == 'X');
                break;
            }
            case 'c':
            {
                char character = (char)va_arg(args, int);

                put_padded(out, &spec, &character, 1);
                break;
            }
            case 's':
            {
                const char *text = va_arg(args, const char *);
                size_t length = 0;

                if (text == NULL)
                {
                    text = "(null)";
                }
                while (text[length] != '\0' &&
                       (spec.precision < 0 || length < (size_t)spec.precision))
                {
                    length++;
                }
                put_padded(out, &spec, text, length);
                break;
            }
            case '%': put(out, "%", 1); break;
            default:
                // Not supported: shown as written, the argument (if any)
                // left alone.
                put(out, start, (size_t)(format - start));
                break;
        }
    }
    va_end(args);
    return out->length > 0x7FFFU ? 0x7FFF : (int)out->length;
}


int format_vsnprintf(char *text, size_t size, const char *format, va_list args)
{
    Out out;
    char none;

    out.text = size != 0U ? text : &none;
    out.size = size;
    out.length = 0;
    (void)format_out(&out, format, args);
    if (size != 0U)
    {
        text[out.length < size ? out.length : size - 1U] = '\0';
    }
    return out.length > 0x7FFFU ? 0x7FFF : (int)out.length;
}


int format_snprintf(char *text, size_t size, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = format_vsnprintf(text, size, format, args);
    va_end(args);
    return length;
}


int format_sprintf(char *text, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = format_vsnprintf(text, 0x7FFFU, format, args);
    va_end(args);
    return length;
}


int format_vprintf(const char *format, va_list args)
{
    Out out;

    out.text = NULL;
    out.size = 0;
    out.length = 0;
    return format_out(&out, format, args);
}


int format_printf(const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = format_vprintf(format, args);
    va_end(args);
    return length;
}
