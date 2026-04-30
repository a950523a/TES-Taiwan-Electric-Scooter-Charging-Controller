#include "services/ota_svc.h"
#include "services/event_bus.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_svc";

static bool s_running    = false;
static int  s_progress   = 0;
static char s_url[256];

static void ota_task(void *arg)
{
    esp_http_client_config_t http_cfg = {
        .url            = s_url,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle;
    if (esp_https_ota_begin(&ota_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    esp_https_ota_get_img_desc(handle, &desc);
    ESP_LOGI(TAG, "new firmware: %s", desc.version);

    while (1) {
        esp_err_t ret = esp_https_ota_perform(handle);
        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int total = esp_https_ota_get_image_size(handle);
            int done  = esp_https_ota_get_image_len_read(handle);
            s_progress = (total > 0) ? (done * 100 / total) : 0;

            charger_event_t evt = { .type = EVT_OTA_PROGRESS };
            evt.payload[0] = (uint8_t)s_progress;
            event_bus_publish(&evt);
        } else {
            break;
        }
    }

    bool ok = esp_https_ota_is_complete_data_received(handle);
    esp_https_ota_finish(handle);

    if (ok) {
        ESP_LOGI(TAG, "OTA complete — rebooting");
        charger_event_t evt = { .type = EVT_OTA_COMPLETE };
        event_bus_publish(&evt);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA incomplete");
    }

    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_svc_start(const char *url)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_running  = true;
    s_progress = 0;

    BaseType_t r = xTaskCreate(ota_task, "ota", 16384, NULL, 2, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool ota_svc_is_running(void)    { return s_running;  }
int  ota_svc_progress_pct(void)  { return s_progress; }
