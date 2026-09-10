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
#include "services/notify_svc.h"
#include "services/log_svc.h"
#include "services/trace_svc.h"
#include "services/mqtt_svc.h"
#include "services/scheduler_svc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

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

static void on_psu_paired(const uint8_t peer_mac[6])
{
    config_svc_set_psu(PSU_TRANSPORT_ESPNOW, peer_mac, true);
}

// ── Global definitions ───────────────────────────────────────────────────────

QueueHandle_t    g_can_rx_queue;
tes_snapshot_t   g_snapshot;
SemaphoreHandle_t g_snapshot_mutex;
atomic_bool      g_emergency_stop;
QueueHandle_t    g_btn_event_queue;
QueueHandle_t    g_display_btn_queue;
volatile bool    g_menu_open = false;
task_entry_t     g_tasks[G_TASK_MAX];
int              g_task_count = 0;

void g_task_unregister_self(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].handle == me) { g_tasks[i].handle = NULL; return; }
    }
}

// 建立任務並登記到 g_tasks，讓 task_monitor 能回報堆疊餘裕
static void spawn(TaskFunction_t fn, const char *name, uint32_t stack, UBaseType_t prio)
{
    TaskHandle_t h = NULL;
    if (xTaskCreate(fn, name, stack, NULL, prio, &h) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(%s) failed", name);
        return;
    }
    if (g_task_count < G_TASK_MAX) {
        g_tasks[g_task_count].name   = name;
        g_tasks[g_task_count].handle = h;
        g_task_count++;
    }
}

// ── Task forward declarations ────────────────────────────────────────────────

void task_can_rx   (void *arg);
void task_tes_sm   (void *arg);
void task_hal_poll (void *arg);
void task_display  (void *arg);
void task_network  (void *arg);
void task_ota      (void *arg);
void task_notify   (void *arg);
void task_log      (void *arg);
void task_mqtt      (void *arg);
void task_scheduler (void *arg);
void task_monitor   (void *arg);

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
    // ADC 缺席不應該讓整台機器開機循環（與 display_driver 一致採非致命處理）。
    // 無 ADS1115 時 CP 與輸出電壓讀值維持 0，仍可用手動 START + PSU 回報值充電。
    if (adc_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 init failed — CP/voltage sensing unavailable");
    }
    ESP_ERROR_CHECK(psu_driver_init());
    display_driver_init();   // non-fatal if OLED absent
    led_driver_init();

    // Services init
    event_bus_init();
    ESP_ERROR_CHECK(config_svc_init());
    notify_svc_init();
    log_svc_init();
    // trace buffer 配置在 PSRAM；失敗只會停用曲線/Log，不影響充電
    if (trace_svc_init() != ESP_OK) {
        ESP_LOGW(TAG, "trace_svc unavailable — charge curve/log disabled");
    }
    mqtt_svc_init();
    ESP_ERROR_CHECK(scheduler_svc_init());

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
    // can_rx 需要 4KB：迴圈裡的 can_driver_service() 在 TWAI 進入 error-passive／
    // bus-off 時會呼叫 ESP_LOGW，而 esp_log 的 vprintf 一次就要 1KB 以上。
    // 2KB 時只要按下 START（開始送 0x508／0x509 而匯流排無 ACK）就必定溢位重開機。
    spawn(task_can_rx,   "can_rx",   4096,  15);
    // tes_sm 需要 8KB：除了 inputs/outputs/snapshot，trace 的變動 Log 會呼叫
    // vsnprintf("%f")，newlib 的浮點格式化本身就要好幾百 bytes。
    // 4KB 時按下 START 會堆疊溢位重開機。
    spawn(task_tes_sm,   "tes_sm",   8192,  12);
    spawn(task_hal_poll, "hal_poll", 4096,  10);
    spawn(task_display,  "display",  4096,   4);
    spawn(task_network,  "network",  12288,  3);
    spawn(task_ota,      "ota",      16384,  2);
    spawn(task_notify,   "notify",   6144,   2);
    spawn(task_mqtt,     "mqtt",     8192,   2);
    spawn(task_scheduler,"sched",    4096,   2);
    spawn(task_log,      "task_log", 3072,   1);
    spawn(task_monitor,  "monitor",  4096,   1);

    network_svc_start();   // 這裡才真正呼叫 esp_wifi_start()

    // ESP-NOW transport init — 必須在 esp_wifi_start() 之後。
    // esp_now_init() 要求 WiFi 已經啟動；放在 network_svc_init() 之後會失敗，
    // psu_driver 會靜默 fallback 回 UART，ESP-NOW 永遠不會生效。
    {
        const charger_config_t *cfg = config_svc_get();
        psu_driver_set_pair_callback(on_psu_paired);
        esp_err_t r = psu_driver_set_transport(
            (psu_transport_t)cfg->psu_transport,
            cfg->psu_paired ? cfg->psu_peer_mac : NULL
        );
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "PSU transport init failed: %s — falling back to UART",
                     esp_err_to_name(r));
        }
    }

    ESP_LOGI(TAG, "all tasks started");
}
