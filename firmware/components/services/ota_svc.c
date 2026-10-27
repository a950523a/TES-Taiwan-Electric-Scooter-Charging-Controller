#include "services/ota_svc.h"
#include "services/event_bus.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_svc";

static volatile ota_state_t      s_state      = OTA_STATE_IDLE;
static volatile ota_source_t     s_source     = OTA_SRC_NONE;
static volatile int              s_progress   = 0;
static char                      s_url[256];
static char                      s_error[128];

static esp_ota_handle_t          s_ota_handle = 0;
static const esp_partition_t    *s_ota_part   = NULL;
static size_t                    s_upload_total = 0;
static size_t                    s_upload_done  = 0;

// ── 兩條路共用 ────────────────────────────────────────────────────────────────

const char *ota_svc_state_name(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_RUNNING:   return "running";
    case OTA_STATE_ERROR:     return "error";
    case OTA_STATE_REBOOTING: return "rebooting";
    default:                  return "idle";
    }
}

const char *ota_svc_source_name(ota_source_t s)
{
    return s == OTA_SRC_ONLINE ? "online" : s == OTA_SRC_UPLOAD ? "upload" : "";
}

static void begin_common(ota_source_t src)
{
    s_source   = src;
    s_progress = 0;
    s_error[0] = '\0';
    s_state    = OTA_STATE_RUNNING;
}

static void fail(const char *fmt, const char *detail)
{
    snprintf(s_error, sizeof(s_error), fmt, detail);
    ESP_LOGE(TAG, "%s", s_error);
    s_state = OTA_STATE_ERROR;
}

// 百分比變了才發事件：每 4 KB 一則的話 1.5 MB 是 ~380 則，每則叫醒所有訂閱的任務
static void set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == s_progress) return;
    s_progress = pct;
    charger_event_t evt = { .type = EVT_OTA_PROGRESS };
    evt.payload[0] = (uint8_t)pct;
    event_bus_publish(&evt);
}

// 這個映像是不是這個產品的韌體：專案名稱要跟現在跑的一樣（例如誤傳 LianMing 的 .bin 進來就擋掉）。
// 晶片型號由 esp_ota_write／esp_https_ota 自己檢查。版本不擋 —— 降版、重灌同一版都是正當的
static bool image_is_ours(const esp_app_desc_t *d)
{
    const esp_app_desc_t *me = esp_app_get_description();
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        fail("%s", "not an ESP-IDF app image");
        return false;
    }
    if (strncmp(d->project_name, me->project_name, sizeof d->project_name) != 0) {
        char p[sizeof d->project_name + 1];
        memcpy(p, d->project_name, sizeof d->project_name);
        p[sizeof d->project_name] = '\0';
        fail("not TES charger firmware (it is \"%s\")", p);
        return false;
    }
    ESP_LOGI(TAG, "image: %s %.32s", d->project_name, d->version);
    return true;
}

static void mark_done(void)
{
    s_progress = 100;
    s_state    = OTA_STATE_REBOOTING;
    ESP_LOGI(TAG, "OTA complete (%s) - rebooting", ota_svc_source_name(s_source));
    charger_event_t evt = { .type = EVT_OTA_COMPLETE };
    event_bus_publish(&evt);
}

void ota_svc_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   // 讓 OLED 畫出「重新啟動」、網頁和 App 讀到 rebooting
    esp_restart();
}

static void reboot_now(void)
{
    mark_done();
    ota_svc_reboot();
}

// ── 線上更新 ──────────────────────────────────────────────────────────────────

static void ota_task(void *arg)
{
    esp_http_client_config_t http_cfg = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms        = 30000,
        /*
         * GitHub 的 releases/latest/download/... 會 302 轉到 release-assets.githubusercontent.com，
         * 2026 年起那個網址帶一串簽章，超過 1000 字元。預設緩衝區 512 B：收不下 Location 標頭、
         * 轉址後的請求行也送不出去，esp_https_ota_begin() 直接 ESP_FAIL（2026-10-14 TP-0001 實測）。
         * buffer_size 收（含標頭行），buffer_size_tx 送（含請求行）。
         */
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        fail("begin: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(handle, &desc) != ESP_OK || !image_is_ours(&desc)) {
        if (s_state != OTA_STATE_ERROR) fail("%s", "cannot read the image header");
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t perform_ret;
    do {
        perform_ret = esp_https_ota_perform(handle);
        if (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int total = esp_https_ota_get_image_size(handle);
            int done  = esp_https_ota_get_image_len_read(handle);
            if (total > 0) set_progress((int)((int64_t)done * 100 / total));
        }
    } while (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (perform_ret != ESP_OK) {
        fail("download: %s", esp_err_to_name(perform_ret));
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t finish_err = esp_https_ota_finish(handle);
    if (finish_err == ESP_OK) {
        reboot_now();
    } else {
        fail("finish: %s", esp_err_to_name(finish_err));
    }
    vTaskDelete(NULL);
}

esp_err_t ota_svc_start(const char *url)
{
    if (ota_svc_busy()) return ESP_ERR_INVALID_STATE;

    const char *target = (url && url[0] != '\0') ? url : OTA_DEFAULT_URL;
    strncpy(s_url, target, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    begin_common(OTA_SRC_ONLINE);

    ESP_LOGI(TAG, "OTA start: %s", s_url);
    BaseType_t r = xTaskCreate(ota_task, "ota", 16384, NULL, 2, NULL);
    if (r != pdPASS) {
        fail("%s", "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

ota_state_t  ota_svc_get_state(void)    { return s_state;    }
ota_source_t ota_svc_source(void)       { return s_source;   }
int          ota_svc_progress_pct(void) { return s_progress; }
const char  *ota_svc_get_error(void)    { return s_error;    }

bool ota_svc_busy(void)
{
    ota_state_t s = s_state;
    return s == OTA_STATE_RUNNING || s == OTA_STATE_REBOOTING;
}

// ── 手動上傳 ──────────────────────────────────────────────────────────────────

esp_err_t ota_svc_upload_begin(size_t total_size)
{
    if (ota_svc_busy()) return ESP_ERR_INVALID_STATE;

    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) {
        begin_common(OTA_SRC_UPLOAD);
        fail("%s", "no OTA partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    begin_common(OTA_SRC_UPLOAD);
    if (err != ESP_OK) {
        fail("begin: %s", esp_err_to_name(err));
        return err;
    }

    s_upload_total = total_size;
    s_upload_done  = 0;
    ESP_LOGI(TAG, "upload begin, total=%u bytes", (unsigned)total_size);
    return ESP_OK;
}

esp_err_t ota_svc_upload_write(const void *data, size_t len)
{
    // 第一塊就看映像描述（映像標頭 24 B ＋ 第一段標頭 8 B 之後）：不是這個產品的韌體就不用傳完才失敗
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (s_upload_done == 0 && len >= desc_off + sizeof(esp_app_desc_t)) {
        esp_app_desc_t d;
        memcpy(&d, (const uint8_t *)data + desc_off, sizeof d);
        if (!image_is_ours(&d)) {
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        fail("write: %s", esp_err_to_name(err));
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        return err;
    }
    s_upload_done += len;
    if (s_upload_total > 0) set_progress((int)((s_upload_done * 100) / s_upload_total));
    return ESP_OK;
}

void ota_svc_upload_abort(const char *why)
{
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    // 以前記成 IDLE：網頁、App、OLED 都看不出更新沒成功
    fail("upload interrupted: %s", why ? why : "connection lost");
}

esp_err_t ota_svc_upload_end(void)
{
    esp_err_t err = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    if (err != ESP_OK) {
        fail("end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_ota_part);
    if (err != ESP_OK) {
        fail("set boot: %s", esp_err_to_name(err));
        return err;
    }
    mark_done();   // 呼叫端回應上傳的人之後呼叫 ota_svc_reboot()
    return ESP_OK;
}
