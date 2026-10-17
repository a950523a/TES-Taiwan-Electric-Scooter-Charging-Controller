#include "globals.h"
#include "services/network_svc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void task_network(void *arg)
{
    (void)arg;
    // network_svc already started in main.c; this task handles event-driven work
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        network_svc_wifi_tick();   // 記住的 WiFi、自動熱點
    }
}
