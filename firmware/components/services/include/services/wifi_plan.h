#pragma once
#include <stdbool.h>
#include <stdint.h>

// 什麼時候掃描找 WiFi、什麼時候開熱點（純函式，可在主機上測，2026-10-17）。
//
// 以前設定過 WiFi 的機器帶出門，會一直重連家裡、永遠不開熱點，手機完全連不上。現在：
//   - 沒有記住的網路，或使用者關掉 STA → 熱點常開、不加密（首次設定要用；跟以前一樣）
//   - 有記住的網路：開機 30 秒內沒連上、或連上後斷線 60 秒 → 開熱點（WPA2，密碼在 OLED）
//     背景照樣掃描：熱點還沒開時每 10 秒、開了之後每 60 秒（掃描要換頻道，連在熱點上的手機會頓一下）
//   - 連上了：熱點上沒有手機超過 30 秒就關掉；有人在用就留著，不把人踢掉
//   - hold（PSU 走 ESP-NOW 且正在充電）：不掃描——換頻道會讓 PSU 連線斷掉而觸發故障
// 熱點開著時不改它的加密方式：設定流程中剛加好網路的手機不能被踢掉。

#define WIFI_PLAN_AP_DELAY_BOOT_MS   30000
#define WIFI_PLAN_AP_DELAY_LOST_MS   60000
#define WIFI_PLAN_SCAN_FAST_MS       10000
#define WIFI_PLAN_SCAN_SLOW_MS       60000
#define WIFI_PLAN_AP_IDLE_OFF_MS     30000
#define WIFI_PLAN_RETRY_MS            2000   // 一組連失敗，換下一組前等這麼久

typedef struct {
    uint32_t now_ms;
    bool     sta_wanted;    // sta_enabled 且至少記住一組網路
    bool     sta_up;        // 連上而且拿到 IP
    bool     sta_busy;      // 掃描或連線中
    bool     hold;          // 不准換頻道
    bool     ap_on;         // 熱點目前開著
    int      ap_clients;
} wifi_plan_in_t;

typedef struct {
    bool scan;              // 現在開始一次掃描
    bool ap_on;             // 熱點應該開著
    bool ap_secure;         // 要開熱點時用 WPA2（熱點已經開著就不理它）
} wifi_plan_out_t;

typedef struct {
    uint32_t down_since_ms;
    uint32_t last_scan_ms;
    uint32_t ap_idle_since_ms;
    uint32_t retry_at_ms;
    bool     retry_pending;
    bool     down_known;
    bool     ever_up;
    bool     scanned;
    bool     ap_idle_known;
} wifi_plan_t;

void wifi_plan_init(wifi_plan_t *p, uint32_t now_ms);
void wifi_plan_tick(wifi_plan_t *p, const wifi_plan_in_t *in, wifi_plan_out_t *out);
// 一組網路連失敗：稍後就再掃一次，換下一組試（不用等完整的掃描間隔）
void wifi_plan_retry_soon(wifi_plan_t *p, uint32_t now_ms);
