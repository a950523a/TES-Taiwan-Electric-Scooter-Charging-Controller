#include "services/event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

#define MAX_SUBSCRIBERS 4
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
        ESP_LOGE(TAG, "subscriber limit reached");
        return NULL;
    }
    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(charger_event_t));
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
