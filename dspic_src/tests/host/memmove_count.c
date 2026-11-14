#include <stddef.h>
unsigned long long memmove_bytes;
void *__real_memmove(void *d, const void *s, size_t n);
void *__wrap_memmove(void *d, const void *s, size_t n)
{
    memmove_bytes += n;
    return __real_memmove(d, s, n);
}
