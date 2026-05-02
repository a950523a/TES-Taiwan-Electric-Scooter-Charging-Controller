#pragma once
#include <esp_err.h>
#include <stdbool.h>

// GitHub Releases: OTA 預設來源（POST /ota 不帶 url 時使用）
#define OTA_DEFAULT_URL \
    "https://github.com/a950523a/TES-Taiwan-Electric-Scooter-Charging-Controller" \
    "/releases/latest/download/tes_charger.bin"

typedef enum {
    OTA_STATE_IDLE    = 0,
    OTA_STATE_RUNNING = 1,
    OTA_STATE_ERROR   = 2,
} ota_state_t;

// url: NULL 或 "" → 使用 OTA_DEFAULT_URL
esp_err_t   ota_svc_start(const char *url);
ota_state_t ota_svc_get_state(void);
int         ota_svc_progress_pct(void);   // 0–100
const char *ota_svc_get_error(void);      // "" 表示無錯誤

// 手動上傳 OTA：由 HTTP handler 逐塊呼叫，end() 內部呼叫 esp_restart()
esp_err_t   ota_svc_upload_begin(size_t total_size);
esp_err_t   ota_svc_upload_write(const void *data, size_t len);
void        ota_svc_upload_abort(void);
esp_err_t   ota_svc_upload_end(void);
