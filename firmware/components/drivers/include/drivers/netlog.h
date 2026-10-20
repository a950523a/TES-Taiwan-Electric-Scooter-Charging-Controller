#pragma once
// WiFi / ESP-NOW 事件紀錄（2026-10-20）：最近 NETLOG_LEN 件，放 RAM，/hw.json 的 "netlog"。
// 為了不接 USB 也看得到「WiFi 什麼時候斷、為什麼、ESP-NOW 跟著怎樣」—— 兩邊互相影響，
// 只看 /status 的最後一次原因碼對不出先後。任何任務（含 WiFi 事件任務）都能呼叫，不印 log、不配置記憶體。
#include <stddef.h>
#include <stdint.h>

#define NETLOG_LEN 48

typedef enum {
    NETLOG_WIFI_TRY = 1,     // a = 頻道（0 = 交給驅動掃），b = 第幾次嘗試
    NETLOG_WIFI_UP,          // a = 頻道
    NETLOG_WIFI_DOWN,        // a = 原因碼（wifi_err_reason_t），b = 1 連線嘗試失敗 / 0 連著時斷線
    NETLOG_WIFI_TIMEOUT,     // 連線逾時，自己斷開
    NETLOG_SCAN_START,
    NETLOG_SCAN_DONE,        // a = 掃到幾個
    NETLOG_AP_ON,            // a = 頻道，b = 1 加密 / 0 開放
    NETLOG_AP_OFF,
    NETLOG_RADIO_BUSY,       // a = 1 開始 / 0 結束（ESP-NOW 暫停握手）
    NETLOG_PSU_UP,
    NETLOG_PSU_DOWN,         // a = 1 逾時 / 2 連續沒有 MAC ACK，b = fail streak
    NETLOG_SESS_READY,       // a = 這次開機第幾次握手成功
    NETLOG_CHN,              // a = 告訴節點的頻道（只記變動）
} netlog_code_t;

void netlog_add(netlog_code_t code, int32_t a, int32_t b);

// 由舊到新，最多 cap 件；回傳件數
typedef struct { uint32_t t_ms; uint8_t code; int32_t a, b; } netlog_entry_t;
size_t netlog_read(netlog_entry_t *out, size_t cap);
const char *netlog_name(uint8_t code);
