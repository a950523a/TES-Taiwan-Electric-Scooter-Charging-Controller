#include "services/notify_svc.h"
#include "services/event_bus.h"
#include "services/config_svc.h"
#include "services/network_svc.h"
#include "services/push_svc.h"
#include "services/live_progress.h"
#include "esp_timer.h"
#include "tes_protocol/tes_types.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// IPC objects owned by main.c — extern, same pattern as network_svc.c
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;

// 由 main.c 提供：自行結束的任務要在 vTaskDelete 前註銷 handle，
// 否則 task_monitor 會讀到懸空指標
extern void g_task_unregister_self(void);

static const char *TAG = "notify_svc";

esp_err_t notify_svc_send(const char *url, const char *title, const char *message, int priority)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"title\":\"%s\",\"message\":\"%s\",\"priority\":%d}",
             title, message, priority);

    esp_http_client_config_t cfg = {
        .url                       = url,
        .method                    = HTTP_METHOD_POST,
        .timeout_ms                = 5000,
        .crt_bundle_attach         = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "http_client_init failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify \"%s\" failed: %s", title, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "notified: %s (HTTP %d)",
                 title, esp_http_client_get_status_code(client));
    }
    esp_err_t result = (err == ESP_OK) ? ESP_OK : ESP_FAIL;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t notify_svc_init(void)
{
    return push_svc_init();
}

// 同一則通知送到兩條路：notify_url（webhook／ntfy）與 App 推播（Expo）。
// 任一條成功就算成功；兩條都沒設定回 ESP_ERR_NOT_FOUND。
esp_err_t notify_svc_broadcast(const char *title, const char *message, int priority)
{
    const char *url = config_svc_get()->notify_url;
    bool any_target = false, any_ok = false;
    if (url[0] != '\0') {
        any_target = true;
        any_ok |= notify_svc_send(url, title, message, priority) == ESP_OK;
    }
    esp_err_t pr = push_svc_send_all(title, message);
    if (pr != ESP_ERR_NOT_FOUND) {
        any_target = true;
        any_ok |= pr == ESP_OK;
    }
    if (!any_target) return ESP_ERR_NOT_FOUND;
    return any_ok ? ESP_OK : ESP_FAIL;
}

// ── App 即時進度（Android Now Bar）────────────────────────────────────────────
// 充電中把進度用「只有資料」的推播送給 App，App 在背景自己更新那則常駐通知；
// 離開充電狀態送一則 live_end 讓 App 收掉它。只送給 App（push_svc），不送 notify_url。
// 每 LIVE_POLL_MS 看一次快照；送不送由 live_should_send() 決定（進度變 1%、至少隔 30 秒）。

#define LIVE_POLL_MS 5000

static struct {
    bool     active;
    int      last_pct;
    uint32_t last_ms;
    uint16_t v0_01;
} s_live;

static void live_tick(void)
{
    if (!network_svc_is_connected() || push_svc_count() == 0) return;

    // static：task_notify 只有 6 KB 堆疊，送推播時 TLS 還要用
    static tes_snapshot_t   snap;
    static charger_config_t cfg;
    static char             data[256];
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    snap = g_snapshot;
    xSemaphoreGive(g_snapshot_mutex);
    config_svc_get_copy(&cfg);

    uint16_t v01 = (uint16_t)(snap.output_voltage * 10.0f + 0.5f);
    if (snap.state != TES_STATE_CHARGING) {
        if (s_live.active) {
            snprintf(data, sizeof data, "{\"k\":\"live_end\",\"id\":\"%s\",\"r\":%u}",
                     cfg.device_id, (unsigned)snap.stop_reason);
            push_svc_send_data_all(data);
            s_live.active = false;
        }
        return;
    }

    bool first = !s_live.active;
    if (first) {
        s_live.active = true;
        s_live.v0_01  = v01;       // 依電壓停止時，進度從這裡起算
    }
    live_inputs_t li = {
        .stop_mode = (uint8_t)cfg.stop_mode,
        .soc       = snap.soc,
        .target_soc = snap.target_soc,
        .v01       = v01,
        .v0_01     = s_live.v0_01,
        .stop_v01  = cfg.stop_voltage_01v,
        .elapsed_s = snap.elapsed_seconds,
        .timer_min = cfg.charge_timer_min,
    };
    int pct = live_progress_pct(&li);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!live_should_send(pct, s_live.last_pct, now, s_live.last_ms, first)) return;

    // 全部用整數：避免 %f 用掉堆疊；App 端把 0.1 V 換回 V
    unsigned w = (unsigned)(snap.output_voltage * snap.output_current + 0.5f);
    snprintf(data, sizeof data,
             "{\"k\":\"live\",\"id\":\"%s\",\"m\":%u,\"p\":%d,\"soc\":%u,\"tsoc\":%d,"
             "\"v\":%u,\"sv\":%u,\"el\":%lu,\"tm\":%u,\"rem\":%lu,\"w\":%u}",
             cfg.device_id, (unsigned)cfg.stop_mode, pct, (unsigned)snap.soc, (int)snap.target_soc,
             (unsigned)v01, (unsigned)cfg.stop_voltage_01v, (unsigned long)snap.elapsed_seconds,
             (unsigned)cfg.charge_timer_min, (unsigned long)snap.remaining_seconds, w);
    // 失敗（例如暫時連不上 Expo）只更新時間：30 秒後再試，不會每 5 秒打一次
    s_live.last_ms = now;
    if (push_svc_send_data_all(data) == ESP_OK || first) s_live.last_pct = pct;
}

void task_notify(void *arg)
{
    (void)arg;
    QueueHandle_t q = event_bus_subscribe();
    if (!q) {
        ESP_LOGE(TAG, "event_bus_subscribe failed — notify disabled");
        g_task_unregister_self();
        vTaskDelete(NULL);
        return;
    }
    charger_event_t evt;
    bool was_charging = false;

    for (;;) {
        bool got = xQueueReceive(q, &evt, pdMS_TO_TICKS(LIVE_POLL_MS)) == pdTRUE;
        live_tick();
        if (!got || evt.type != EVT_TES_STATE_CHANGED) continue;

        if (!network_svc_is_connected()) continue;   // AP 模式或尚未連線：跳過

        if (config_svc_get()->notify_url[0] == '\0' && push_svc_count() == 0) continue;

        tes_state_t new_state = (tes_state_t)evt.payload[0];

        if (new_state == TES_STATE_CHARGING && !was_charging) {
            notify_svc_broadcast("充電開始", "充電器已連接並開始充電", 3);
            was_charging = true;

        } else if (new_state == TES_STATE_IDLE && was_charging) {
            // Read snapshot for SOC + elapsed time right after state transition
            tes_snapshot_t snap = {0};
            if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snap = g_snapshot;
                xSemaphoreGive(g_snapshot_mutex);
            }
            if (snap.charge_complete) {
                char msg[64];
                uint32_t h = snap.elapsed_seconds / 3600;
                uint32_t m = (snap.elapsed_seconds % 3600) / 60;
                if (h > 0) {
                    snprintf(msg, sizeof(msg), "SOC %d%%  %luh %02lum", snap.soc, (unsigned long)h, (unsigned long)m);
                } else {
                    snprintf(msg, sizeof(msg), "SOC %d%%  %lum", snap.soc, (unsigned long)m);
                }
                notify_svc_broadcast("充電完成", msg, 3);
            }
            was_charging = false;

        } else if (new_state == TES_STATE_FAULT) {
            notify_svc_broadcast("充電故障", "請確認設備狀態", 4);
            was_charging = false;

        } else if (new_state == TES_STATE_EMERGENCY) {
            notify_svc_broadcast("緊急停止", "充電器觸發緊急停止", 5);
            was_charging = false;

        } else if (new_state == TES_STATE_IDLE) {
            was_charging = false;
        }
    }
}
