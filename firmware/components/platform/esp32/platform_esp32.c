#include "platform/platform.h"
#include <esp_timer.h>

uint32_t platform_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}
