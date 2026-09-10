#include "globals.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "monitor";

// 低於這個值就示警。堆疊溢位在 ESP32 上是直接 abort 重開機，
// 事後很難查，所以要在還有餘裕時就先叫。
#define STACK_WARN_BYTES 512

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

        // 各任務的堆疊歷史最低餘裕（bytes）
        char line[224];
        int  n = 0;
        for (int i = 0; i < g_task_count; i++) {
            if (!g_tasks[i].handle) continue;
            // uxTaskGetStackHighWaterMark 回傳的單位是 word，換算成 bytes
            unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(g_tasks[i].handle)
                              * sizeof(StackType_t);
            if (n < (int)sizeof(line) - 24)
                n += snprintf(line + n, sizeof(line) - n, "%s=%u ", g_tasks[i].name, free_b);
            if (free_b < STACK_WARN_BYTES) {
                ESP_LOGW(TAG, "task \"%s\" stack low: %u bytes free — raise its stack size",
                         g_tasks[i].name, free_b);
            }
        }
        if (n > 0) ESP_LOGI(TAG, "stack free: %s", line);
    }
}
