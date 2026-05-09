// scheduler_svc.c — Daily charging window scheduler with NTP time sync (UTC+8)
//
// Fires EVT_BUTTON_START / EVT_BUTTON_STOP to g_btn_event_queue based on
// configured start/stop times (minutes from midnight).
//
// Edge-crossing detection: tracks last-checked minute so each time boundary
// fires exactly once per day, even if the task wakes up slightly late.
//
// Overnight windows (start_min > stop_min, e.g. 23:00–06:00) are supported.
//
// On first NTP sync after boot: if stop is enabled and current time is already
// inside the window, immediately fires START to recover from a reboot that
// happened mid-window.

#include "services/scheduler_svc.h"
#include "services/config_svc.h"
#include "services/event_bus.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <string.h>

static const char *TAG = "scheduler_svc";

extern QueueHandle_t g_btn_event_queue;

static bool s_sntp_started = false;
static bool s_ntp_synced   = false;
static char s_time_str[20] = "---";   // "YYYY-MM-DD HH:MM"
static int  s_last_minute  = -1;      // -1 = first sync not yet processed

// ── Internal helpers ──────────────────────────────────────────────────────────

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
    }
}

// Returns true if target was crossed in the interval (from, to], handling
// midnight wrap when from > to (task slept across midnight).
static bool crossed(int from, int to, int target)
{
    if (from < to)  return target > from && target <= to;
    return target > from || target <= to;   // midnight crossing
}

// Returns true if cur is inside [start_m, stop_m) — overnight-aware.
static bool in_window(int cur, int start_m, int stop_m)
{
    if (start_m <= stop_m) return cur >= start_m && cur < stop_m;
    return cur >= start_m || cur < stop_m;  // overnight: start > stop
}

static void fire(uint8_t evt)
{
    xQueueSendToBack(g_btn_event_queue, &evt, 0);
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t scheduler_svc_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to UTC+8");
    return ESP_OK;
}

void scheduler_svc_get_time_info(bool *synced_out, char *buf, size_t buflen)
{
    if (synced_out) *synced_out = s_ntp_synced;
    if (buf && buflen > 0) {
        strncpy(buf, s_time_str, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

// ── Task ──────────────────────────────────────────────────────────────────────

void task_scheduler(void *arg)
{
    QueueHandle_t bus = event_bus_subscribe();

    // Start SNTP immediately — it will sync once WiFi connects.
    start_sntp();

    for (;;) {
        // Drain event bus: restart SNTP if WiFi connected while we slept.
        charger_event_t evt;
        while (xQueueReceive(bus, &evt, 0) == pdTRUE) {
            if (evt.type == EVT_WIFI_CHANGED) {
                start_sntp();
            }
        }

        // Check NTP sync status.
        if (s_sntp_started) {
            s_ntp_synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
        }

        if (s_ntp_synced) {
            time_t now;
            struct tm tm;
            time(&now);
            localtime_r(&now, &tm);
            strftime(s_time_str, sizeof(s_time_str), "%Y-%m-%d %H:%M", &tm);

            int cur_minute = tm.tm_hour * 60 + tm.tm_min;
            const charger_config_t *cfg = config_svc_get();

            if (cfg->sched_enabled) {
                if (s_last_minute == -1) {
                    // First sync after boot: if inside window (stop enabled), resume charging.
                    if (cfg->sched_stop_en &&
                        in_window(cur_minute, cfg->sched_start_min, cfg->sched_stop_min)) {
                        ESP_LOGI(TAG, "boot inside window (%02d:%02d–%02d:%02d), firing START",
                                 cfg->sched_start_min / 60, cfg->sched_start_min % 60,
                                 cfg->sched_stop_min  / 60, cfg->sched_stop_min  % 60);
                        fire(EVT_BUTTON_START);
                    }
                } else if (s_last_minute != cur_minute) {
                    if (crossed(s_last_minute, cur_minute, cfg->sched_start_min)) {
                        ESP_LOGI(TAG, "schedule START at %02d:%02d",
                                 cfg->sched_start_min / 60, cfg->sched_start_min % 60);
                        fire(EVT_BUTTON_START);
                    }
                    if (cfg->sched_stop_en &&
                        crossed(s_last_minute, cur_minute, cfg->sched_stop_min)) {
                        ESP_LOGI(TAG, "schedule STOP at %02d:%02d",
                                 cfg->sched_stop_min / 60, cfg->sched_stop_min % 60);
                        fire(EVT_BUTTON_STOP);
                    }
                }
            }

            s_last_minute = cur_minute;
        } else {
            strncpy(s_time_str, "---", sizeof(s_time_str));
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
