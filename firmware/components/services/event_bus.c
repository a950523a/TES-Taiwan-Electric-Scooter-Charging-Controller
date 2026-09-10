#include "services/event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

// 目前的訂閱者：task_ota / task_notify / task_mqtt / task_scheduler / task_log = 5。
// 上限 4 曾導致（啟用 MQTT 時）最後一個訂閱者拿到 NULL queue 而 panic。
// 新增訂閱者時務必同步調整這個值。
#define MAX_SUBSCRIBERS 8
#define QUEUE_DEPTH     8

static const char *TAG = "event_bus";

static QueueHandle_t s_queues[MAX_SUBSCRIBERS];
static int           s_sub_count = 0;

void event_bus_init(void)
{
    s_sub_count = 0;
    memset(s_queues, 0, sizeof(s_queues));
}

QueueHandle_t event_bus_subscribe(void)
{
    if (s_sub_count >= MAX_SUBSCRIBERS) {
        ESP_LOGE(TAG, "subscriber limit (%d) reached — raise MAX_SUBSCRIBERS",
                 MAX_SUBSCRIBERS);
        return NULL;
    }
    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(charger_event_t));
    if (!q) {
        ESP_LOGE(TAG, "queue alloc failed");
        return NULL;
    }
    s_queues[s_sub_count++] = q;
    return q;
}

void event_bus_publish(const charger_event_t *evt)
{
    for (int i = 0; i < s_sub_count; i++) {
        if (s_queues[i]) {
            // Non-blocking: drop the event if the subscriber is slow
            xQueueSendToBack(s_queues[i], evt, 0);
        }
    }
}
