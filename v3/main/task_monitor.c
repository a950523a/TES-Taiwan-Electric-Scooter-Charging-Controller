#include "globals.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "monitor";

void task_monitor(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        size_t free_heap  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t min_heap   = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "heap free=%u min=%u psram=%u",
                 (unsigned)free_heap, (unsigned)min_heap, (unsigned)free_psram);

        // Log per-task stack watermarks
        TaskStatus_t tasks[12];
        UBaseType_t n = uxTaskGetSystemState(tasks, 12, NULL);
        for (UBaseType_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  %-12s stack_hwm=%u",
                     tasks[i].pcTaskName,
                     (unsigned)tasks[i].usStackHighWaterMark);
        }
    }
}
