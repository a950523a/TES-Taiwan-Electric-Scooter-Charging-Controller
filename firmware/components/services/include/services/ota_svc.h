#pragma once
#include <esp_err.h>
#include <stdbool.h>

// GitHub Releases: OTA 預設來源（POST /ota 不帶 url 時使用）
#define OTA_DEFAULT_URL \
    "https://github.com/a950523a/TES-Taiwan-Electric-Scooter-Charging-Controller" \
    "/releases/latest/download/tes_charger.bin"

// 兩條路（線上下載、手動上傳）共用同一套狀態（2026-10-27）：OLED、網頁、App 不管更新是從哪裡
// 觸發的，看到的都一樣
typedef enum {
    OTA_STATE_IDLE      = 0,
    OTA_STATE_RUNNING   = 1,
    OTA_STATE_ERROR     = 2,   // 失敗：原本的韌體照常運作，錯誤留到下一次更新
    OTA_STATE_REBOOTING = 3,   // 寫完、驗證過，1.5 s 後重新啟動
} ota_state_t;

typedef enum {
    OTA_SRC_NONE   = 0,
    OTA_SRC_ONLINE = 1,   // POST /ota：從 GitHub（或指定網址）下載
    OTA_SRC_UPLOAD = 2,   // POST /ota/upload：網頁或 App 上傳 .bin
} ota_source_t;

// url: NULL 或 "" → 使用 OTA_DEFAULT_URL
esp_err_t    ota_svc_start(const char *url);
ota_state_t  ota_svc_get_state(void);
ota_source_t ota_svc_source(void);        // 正在跑或最近一次的來源
bool         ota_svc_busy(void);          // RUNNING 或 REBOOTING：這段時間不能開始充電
int          ota_svc_progress_pct(void);  // 0–100
const char  *ota_svc_get_error(void);     // "" 表示無錯誤
const char  *ota_svc_state_name(ota_state_t s);    // "idle" / "running" / "rebooting" / "error"
const char  *ota_svc_source_name(ota_source_t s);  // "" / "online" / "upload"

// 手動上傳 OTA：由 HTTP handler 逐塊呼叫。end() 驗證映像、設好開機分區（狀態變 rebooting）但不重開：
// 呼叫端先回應上傳的人（成功或失敗原因），再呼叫 ota_svc_reboot()
esp_err_t   ota_svc_upload_begin(size_t total_size);
esp_err_t   ota_svc_upload_write(const void *data, size_t len);
void        ota_svc_upload_abort(const char *why);   // 收不到資料、連線斷了：記成錯誤
esp_err_t   ota_svc_upload_end(void);
void        ota_svc_reboot(void);                    // 等 1.5 s 讓畫面與輪詢跟上，再重新啟動
