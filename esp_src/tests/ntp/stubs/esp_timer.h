#ifndef ESP_TIMER_H_STUB
#define ESP_TIMER_H_STUB
#include <stdint.h>
extern int64_t fake_now_us;
static inline int64_t esp_timer_get_time(void)
{
    return fake_now_us;
}
#endif
