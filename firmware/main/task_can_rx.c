#include "globals.h"
#include "drivers/can_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "task_can_rx";

// Blocks on TWAI receive, forwards raw frames to g_can_rx_queue.
// task_tes_sm decodes them in its own context to keep this task minimal.
void task_can_rx(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "started");
    while (1) {
        can_frame_t frame;
        if (can_driver_receive(&frame, 100)) {
            if (xQueueSendToBack(g_can_rx_queue, &frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "CAN RX queue full — frame 0x%03lX dropped", (unsigned long)frame.id);
            }
        }
    }
}
