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
    while (1) {
        charger_event_t evt;
        if (xQueueReceive(q, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.type == EVT_OTA_COMPLETE) {
                // esp_restart() is called inside ota_svc
            }
        }
    }
}
