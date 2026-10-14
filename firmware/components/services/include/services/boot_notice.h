#pragma once
#include <stdbool.h>
#include <stddef.h>

// 重開機後的補發通知（純函式，可在主機上測）。
//
// 當機、看門狗、電壓不足、斷電時，控制器來不及送「充電中斷」—— 重開機後依「重啟原因」
// 與「重開前是否在充電」（NVS 標記）決定要不要補發、說什麼：
//   重開前在充電           → 一律通知「充電中斷」（任何原因都是），並請 App 收掉進度通知
//   沒在充電、異常重啟     → 通知「控制器異常重啟」（當機、看門狗、電壓不足）
//   沒在充電、正常重開     → 不通知（斷電復電、軟體重開、EN 鍵）

typedef enum {
    BOOT_RST_POWERON,   // 斷電後復電
    BOOT_RST_EXT,       // 外部重置（EN 鍵）
    BOOT_RST_SW,        // esp_restart()：OTA、使用者按重新啟動
    BOOT_RST_PANIC,     // 當機
    BOOT_RST_WDT,       // 看門狗（int／task／rtc）
    BOOT_RST_BROWNOUT,  // 電源電壓不足
    BOOT_RST_OTHER,
} boot_rst_t;

// 重開前在充電：soc_start 是開始時的 SOC，未知給 -1。
// 要通知回 true 並填好 title／body（UTF-8，不含 " 與 \，可直接放進 JSON）。
bool boot_notice_build(boot_rst_t rst, bool was_charging, int soc_start,
                       char *title, size_t title_len, char *body, size_t body_len);

const char *boot_rst_label(boot_rst_t rst);
