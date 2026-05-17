#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// PSU 文字協議（UART 和 ESP-NOW 共用相同格式）
// RX:  "V=xx.x,I=xx.x\n"
// TX:  "SET:V=xx.x\n" / "SET:I=xx.x\n"

typedef struct {
    float   voltage;      // V
    float   current;      // A
    bool    connected;
    int8_t  rssi;         // ESP-NOW: 最後收到的 RSSI (dBm)；UART 為 0
    uint8_t fail_streak;  // ESP-NOW: 連續 MAC-ACK 失敗次數；UART 為 0
} psu_status_t;

typedef enum {
    PSU_TRANSPORT_UART   = 0,   // 有線 UART（預設）
    PSU_TRANSPORT_ESPNOW = 1,   // 無線 ESP-NOW
} psu_transport_t;

// 配對完成時的回呼，peer_mac 為 PSU 的 MAC 地址（6 bytes）
typedef void (*psu_pair_done_cb_t)(const uint8_t peer_mac[6]);

esp_err_t    psu_driver_init        (void);
void         psu_driver_poll        (void);   // 非阻塞，由 task_hal_poll 每 tick 呼叫
void         psu_driver_set_voltage (float v);
void         psu_driver_set_current (float a);
psu_status_t psu_driver_get_status  (void);

// 在 network_svc_init() 之後呼叫（ESP-NOW 需要 WiFi 已啟動）
// peer_mac_6 = NULL 表示尚未配對
esp_err_t psu_driver_set_transport(psu_transport_t t, const uint8_t *peer_mac_6);

// 設定配對完成的持久回呼（會在每次配對成功時呼叫）
void psu_driver_set_pair_callback(psu_pair_done_cb_t cb);

// 啟動 10 秒配對視窗；cb 可為 NULL（使用已設定的持久回呼）
void psu_driver_start_pairing(psu_pair_done_cb_t cb);
bool psu_driver_is_pairing(void);

// ESP-NOW 專用：是否已有已配對的 peer（transport=UART 時永遠回傳 false）
bool psu_driver_has_peer(void);
