#include "globals.h"
#include "services/display_svc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void task_display(void *arg)
{
    (void)arg;
    display_svc_init();
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));
        display_svc_tick();
    }
}
