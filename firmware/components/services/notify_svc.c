#include "services/notify_svc.h"
#include "services/event_bus.h"
#include "services/config_svc.h"
#include "services/network_svc.h"
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
    return ESP_OK;
}

void task_notify(void *arg)
{
    (void)arg;
    QueueHandle_t q = event_bus_subscribe();
    charger_event_t evt;
    bool was_charging = false;

    for (;;) {
        if (xQueueReceive(q, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.type != EVT_TES_STATE_CHANGED) continue;

        if (!network_svc_is_connected()) continue;   // AP 模式或尚未連線：跳過

        const char *url = config_svc_get()->notify_url;
        if (url[0] == '\0') continue;

        tes_state_t new_state = (tes_state_t)evt.payload[0];

        if (new_state == TES_STATE_CHARGING && !was_charging) {
            notify_svc_send(url, "充電開始", "充電器已連接並開始充電", 3);
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
                notify_svc_send(url, "充電完成", msg, 3);
            }
            was_charging = false;

        } else if (new_state == TES_STATE_FAULT) {
            notify_svc_send(url, "充電故障", "請確認設備狀態", 4);
            was_charging = false;

        } else if (new_state == TES_STATE_EMERGENCY) {
            notify_svc_send(url, "緊急停止", "充電器觸發緊急停止", 5);
            was_charging = false;

        } else if (new_state == TES_STATE_IDLE) {
            was_charging = false;
        }
    }
}
