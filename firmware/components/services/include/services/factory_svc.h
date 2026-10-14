#pragma once
// factory_svc — 出廠資料：原廠板出廠時寫進 tes_factory 分區、用作者私鑰簽名的一小塊資料
//
// 目前唯一的用途：/hw 頁面的電路板圖。生產板的圖不在公開韌體裡，只存在原廠板的
// 出廠資料中；驗證通過才給。格式與燒錄工具在私有的硬體 repo
// （tools/provision_factory.py），兩邊的結構必須一致。
//
// 驗證：簽名（ECDSA P-256，公鑰內建在 factory_svc.c）＋ MAC 必須是這顆晶片的
// ＋ 電路板圖的 SHA-256。改過的韌體當然可以跳過這些 —— 這不是安全機制，只決定
// 官方韌體要不要顯示那張圖。
//
// 舊機（沒有 tes_factory 分區、或分區是空的）→ FACTORY_NONE，不是 INVALID。

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FACTORY_NONE = 0,     // 沒有分區或分區是空的：2026-10 以前燒錄的板子、或沒做出廠燒錄
    FACTORY_VALID,        // 簽名、MAC、圖的雜湊都對
    FACTORY_INVALID,      // 有資料但驗不過（reason 說明哪一項）
} factory_state_t;

typedef struct {
    factory_state_t state;
    const char     *reason;        // "ok"、"no partition"、"blank"、"mac mismatch"…
    char            serial[16];    // 出廠序號（VALID 時有效）
    uint8_t         hw_level;      // 出廠時登記的 AIN3 級數
    uint32_t        made_unix;     // 出廠燒錄時間
    uint32_t        drawing_len;   // 電路板圖大小（gzip 後）
} factory_info_t;

// 第一次呼叫時讀分區並驗證（ECDSA 要幾 KB 堆疊 —— 只從 httpd 的工作執行緒呼叫，
// 不要在 app_main 裡呼叫：主任務堆疊只有 3.5 KB）。之後回傳快取。
const factory_info_t *factory_svc_get(void);

// 讀電路板圖（gzip 過的 SVG）。只在 VALID 時可用。
esp_err_t factory_svc_read_drawing(size_t offset, void *buf, size_t len);
