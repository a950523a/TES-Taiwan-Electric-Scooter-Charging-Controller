#include "globals.h"
#include "services/display_svc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

void task_display(void *arg)
{
    (void)arg;
    display_svc_init();
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));

        // Process button events from hal_poll before rendering
        uint8_t btn;
        while (xQueueReceive(g_display_btn_queue, &btn, 0) == pdTRUE) {
            display_svc_button(btn);
        }

        display_svc_tick();
    }
}
