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

#define AUTO_VOLT_SETTLE_MS  1000   // ADC 穩定等待時間
#define AUTO_VOLT_MIN_V      40.0f
#define AUTO_VOLT_MAX_V     120.0f

// ── Boot logo (auto-voltage settle screen) ───────────────────────────────────
// Turtle mark on left (cx=30, cy=36), "Auto Setting Voltage..." on right.
// Shell: double circle ring. Lightning bolt inside. Head, 4 flippers, tail.

static void draw_auto_volt_screen(void)
{
    if (!display_driver_is_ok()) return;
    display_driver_clear();
    display_driver_set_color(1);

    // Shell — double ring
    display_driver_draw_circle(30, 36, 17);
    display_driver_draw_circle(30, 36, 13);

    // Head (filled disc)
    display_driver_draw_disc(30, 10, 5);

    // Neck
    display_driver_draw_line(30, 15, 30, 19);
    display_driver_draw_line(29, 15, 29, 19);

    // Flippers (filled triangles)
    display_driver_draw_triangle(13, 25,  0, 17,  2, 29);  // upper-left
    display_driver_draw_triangle(13, 47,  0, 39,  2, 51);  // lower-left
    display_driver_draw_triangle(47, 25, 60, 17, 58, 29);  // upper-right
    display_driver_draw_triangle(47, 47, 60, 39, 58, 51);  // lower-right

    // Tail
    display_driver_draw_triangle(30, 54, 25, 63, 35, 63);

    // Lightning bolt — two filled triangles forming a Z-zigzag
    // Upper part: top-right → middle-left → middle step
    display_driver_draw_triangle(35, 24, 27, 35, 31, 35);
    // Lower part: middle step → bottom
    display_driver_draw_triangle(27, 35, 31, 35, 23, 46);

    // Text (right side)
    display_driver_font_bold();
    display_driver_draw_str(65, 20, "Auto");
    display_driver_draw_str(65, 36, "Setting");
    display_driver_draw_str(65, 52, "Voltage...");

    display_driver_flush();
}

static const char *TAG = "main";

// ── Global definitions ───────────────────────────────────────────────────────

QueueHandle_t    g_can_rx_queue;
tes_snapshot_t   g_snapshot;
SemaphoreHandle_t g_snapshot_mutex;
atomic_bool      g_emergency_stop;
QueueHandle_t    g_btn_event_queue;
QueueHandle_t    g_display_btn_queue;
volatile bool    g_menu_open = false;

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
    ESP_ERROR_CHECK(hal_uart_psu_init());

    // Driver init
    ESP_ERROR_CHECK(can_driver_init());
    ESP_ERROR_CHECK(adc_driver_init());
    ESP_ERROR_CHECK(psu_driver_init());
    display_driver_init();   // non-fatal if OLED absent
    led_driver_init();

    // Services init
    event_bus_init();
    ESP_ERROR_CHECK(config_svc_init());

    // Auto-voltage: 等待 ADC 穩定後讀一次電壓，更新 max_voltage（僅 RAM）
    if (config_svc_get()->auto_voltage) {
        ESP_LOGI(TAG, "auto-voltage: waiting %dms for ADC to settle", AUTO_VOLT_SETTLE_MS);
        draw_auto_volt_screen();
        vTaskDelay(pdMS_TO_TICKS(AUTO_VOLT_SETTLE_MS));
        float v = adc_driver_read_voltage();
        if (v >= AUTO_VOLT_MIN_V && v <= AUTO_VOLT_MAX_V) {
            config_svc_override_voltage((uint16_t)(v * 10.0f + 0.5f));
        } else {
            ESP_LOGW(TAG, "auto-voltage: ADC read %.1fV out of range, keeping NVS value", v);
        }
    }

    ESP_ERROR_CHECK(network_svc_init());

    // IPC objects
    g_can_rx_queue      = xQueueCreate(16, sizeof(can_frame_t));
    g_snapshot_mutex    = xSemaphoreCreateMutex();
    g_btn_event_queue   = xQueueCreate(8, sizeof(uint8_t));
    g_display_btn_queue = xQueueCreate(8, sizeof(uint8_t));
    atomic_init(&g_emergency_stop, false);

    // Spawn tasks (priority 15 = highest used here)
    xTaskCreate(task_can_rx,   "can_rx",   2048,  NULL, 15, NULL);
    xTaskCreate(task_tes_sm,   "tes_sm",   4096,  NULL, 12, NULL);
    xTaskCreate(task_hal_poll, "hal_poll", 4096,  NULL, 10, NULL);
    xTaskCreate(task_display,  "display",  4096,  NULL,  4, NULL);
    xTaskCreate(task_network,  "network",  12288, NULL,  3, NULL);
    xTaskCreate(task_ota,      "ota",      16384, NULL,  2, NULL);
    xTaskCreate(task_monitor,  "monitor",  4096,  NULL,  1, NULL);

    network_svc_start();

    ESP_LOGI(TAG, "all tasks started");
}
