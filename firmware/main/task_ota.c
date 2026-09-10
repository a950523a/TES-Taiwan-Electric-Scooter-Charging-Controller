#include "globals.h"
#include "services/ota_svc.h"
#include "services/event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void task_ota(void *arg)
{
    (void)arg;
    // OTA is triggered via REST POST /ota → ota_svc_start(url)
    // This task monitors the event bus for OTA completion
    QueueHandle_t q = event_bus_subscribe();
    if (!q) {
        // 訂閱失敗（event bus 已滿）：安靜退出，不要拿 NULL 去呼叫 queue API
        g_task_unregister_self();
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        charger_event_t evt;
        if (xQueueReceive(q, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.type == EVT_OTA_COMPLETE) {
                // esp_restart() is called inside ota_svc
            }
        }
    }
}
