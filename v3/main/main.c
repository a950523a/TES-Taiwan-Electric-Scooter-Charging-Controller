#include "globals.h"
#include "hal/hal_gpio.h"
#include "hal/hal_i2c.h"
#include "hal/hal_uart.h"
#include "hal/hal_nvs.h"
#include "drivers/can_driver.h"
#include "drivers/adc_driver.h"
#include "drivers/psu_driver.h"
#include "drivers/display_driver.h"
#include "drivers/led_driver.h"
#include "services/event_bus.h"
#include "services/config_svc.h"
#include "services/network_svc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "main";

// ── Global definitions ───────────────────────────────────────────────────────

QueueHandle_t    g_can_rx_queue;
tes_snapshot_t   g_snapshot;
SemaphoreHandle_t g_snapshot_mutex;
atomic_bool      g_emergency_stop;
QueueHandle_t    g_btn_event_queue;

// ── Task forward declarations ────────────────────────────────────────────────

void task_can_rx   (void *arg);
void task_tes_sm   (void *arg);
void task_hal_poll (void *arg);
void task_display  (void *arg);
void task_network  (void *arg);
void task_ota      (void *arg);
void task_monitor  (void *arg);

// ── Entry point ──────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "TES Charger V3 starting");

    // HAL init
    ESP_ERROR_CHECK(hal_nvs_init());
    hal_gpio_init();
    ESP_ERROR_CHECK(hal_i2c_init());
    ESP_ERROR_CHECK(hal_uart_init());

    // Driver init
    ESP_ERROR_CHECK(can_driver_init());
    ESP_ERROR_CHECK(adc_driver_init());
    ESP_ERROR_CHECK(psu_driver_init());
    display_driver_init();   // non-fatal if OLED absent
    led_driver_init();

    // Services init
    event_bus_init();
    ESP_ERROR_CHECK(config_svc_init());
    ESP_ERROR_CHECK(network_svc_init());

    // IPC objects
    g_can_rx_queue    = xQueueCreate(16, sizeof(can_frame_t));
    g_snapshot_mutex  = xSemaphoreCreateMutex();
    g_btn_event_queue = xQueueCreate(8, sizeof(uint8_t));
    atomic_init(&g_emergency_stop, false);

    // Spawn tasks (priority 15 = highest used here)
    xTaskCreate(task_can_rx,   "can_rx",   2048,  NULL, 15, NULL);
    xTaskCreate(task_tes_sm,   "tes_sm",   4096,  NULL, 12, NULL);
    xTaskCreate(task_hal_poll, "hal_poll", 2048,  NULL, 10, NULL);
    xTaskCreate(task_display,  "display",  4096,  NULL,  4, NULL);
    xTaskCreate(task_network,  "network",  12288, NULL,  3, NULL);
    xTaskCreate(task_ota,      "ota",      16384, NULL,  2, NULL);
    xTaskCreate(task_monitor,  "monitor",  2048,  NULL,  1, NULL);

    network_svc_start();

    ESP_LOGI(TAG, "all tasks started");
}
