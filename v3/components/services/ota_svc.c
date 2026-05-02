#include "services/ota_svc.h"
#include "services/event_bus.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_svc";

static volatile ota_state_t s_state    = OTA_STATE_IDLE;
static volatile int         s_progress = 0;
static char                 s_url[256];
static char                 s_error[128];

static void ota_task(void *arg)
{
    esp_http_client_config_t http_cfg = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms        = 30000,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "begin: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_error);
        s_state = OTA_STATE_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(handle, &desc) == ESP_OK)
        ESP_LOGI(TAG, "downloading firmware %s", desc.version);

    esp_err_t perform_ret;
    do {
        perform_ret = esp_https_ota_perform(handle);
        if (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int total = esp_https_ota_get_image_size(handle);
            int done  = esp_https_ota_get_image_len_read(handle);
            s_progress = (total > 0) ? (done * 100 / total) : 0;

            charger_event_t evt = { .type = EVT_OTA_PROGRESS };
            evt.payload[0] = (uint8_t)s_progress;
            event_bus_publish(&evt);
        }
    } while (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (perform_ret != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "download: %s", esp_err_to_name(perform_ret));
        ESP_LOGE(TAG, "%s", s_error);
        s_state = OTA_STATE_ERROR;
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t finish_err = esp_https_ota_finish(handle);
    if (finish_err == ESP_OK) {
        s_progress = 100;
        ESP_LOGI(TAG, "OTA complete — rebooting");
        charger_event_t evt = { .type = EVT_OTA_COMPLETE };
        event_bus_publish(&evt);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        snprintf(s_error, sizeof(s_error), "finish: %s", esp_err_to_name(finish_err));
        ESP_LOGE(TAG, "%s", s_error);
        s_state = OTA_STATE_ERROR;
    }

    vTaskDelete(NULL);
}

esp_err_t ota_svc_start(const char *url)
{
    if (s_state == OTA_STATE_RUNNING) return ESP_ERR_INVALID_STATE;

    const char *target = (url && url[0] != '\0') ? url : OTA_DEFAULT_URL;
    strncpy(s_url, target, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_error[0]  = '\0';
    s_progress  = 0;
    s_state     = OTA_STATE_RUNNING;

    ESP_LOGI(TAG, "OTA start: %s", s_url);
    BaseType_t r = xTaskCreate(ota_task, "ota", 16384, NULL, 2, NULL);
    if (r != pdPASS) {
        s_state = OTA_STATE_ERROR;
        snprintf(s_error, sizeof(s_error), "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

ota_state_t ota_svc_get_state(void)    { return s_state;    }
int         ota_svc_progress_pct(void) { return s_progress; }
const char *ota_svc_get_error(void)    { return s_error;    }
