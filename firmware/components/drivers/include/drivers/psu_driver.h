#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include "psu_link/psu_link.h"

// 電源節點連線（UART 或 ESP-NOW）。線上格式由 PSU-Link 子模組（psu_link/psu_link.h）定義：
//   節點 → 控制板   $CAP / $ST / $ACK
//   控制板 → 節點   $HELO / $SET
// 不以 '$' 開頭的行（節點的開機訊息、人下的文字指令回應…）一律忽略。

typedef struct {
    // ── 狀態機、顯示、網頁用的欄位（名稱與意義與舊版相同）──────────────────
    float   voltage;      // V；最新 ST 的電壓無效（或已斷線）時為 0
    float   current;      // A；最新 ST 的電流無效（或已斷線）時為 0
    bool    connected;    // 逾時內收到過有效訊框
    int8_t  rssi;         // ESP-NOW: 最後收到的 RSSI (dBm)；UART 為 0
    uint8_t fail_streak;  // ESP-NOW: 連續 MAC-ACK 失敗次數；UART 為 0

    // ── 節點身分（收到 CAP 之後才有效；斷線時清除）──────────────────────────
    bool     caps_known;
    uint8_t  proto_ver;
    uint8_t  node_type;   // psu_node_type_t
    uint16_t caps;        // PSU_CAP_*
    uint16_t v_max_cv;    // 0.01 V，0 = 節點沒說
    uint16_t i_max_ca;    // 0.01 A，0 = 節點沒說
    uint16_t fw_ver;

    // ── 最新一筆 ST ──────────────────────────────────────────────────────────
    uint8_t  mode;        // psu_mode_t
    uint8_t  flags;       // PSU_ST_*
    uint16_t status_seq;
    uint32_t status_age_ms;   // 距離收到最新一筆 ST；沒收過為 UINT32_MAX

    // ── SET 往返 ─────────────────────────────────────────────────────────────
    uint16_t last_set_seq;    // 最後送出的 SET
    uint16_t last_ack_seq;    // 最後收到的 ACK
    uint8_t  last_ack_result; // psu_ack_result_t

    // ── 連線診斷（開機起累計）────────────────────────────────────────────────
    uint32_t rx_frames;       // 解碼成功的訊框
    uint32_t rx_crc_errors;   // CRC 錯誤 —— 持續上升代表線路雜訊或鮑率不對
    uint32_t rx_bad_frames;   // CRC 對但內容不認得（類型或欄位不對，多半是版本不一致）
} psu_status_t;

typedef enum {
    PSU_TRANSPORT_UART   = 0,   // 有線 UART（預設）
    PSU_TRANSPORT_ESPNOW = 1,   // 無線 ESP-NOW
} psu_transport_t;

// 配對完成時的回呼，peer_mac 為 PSU 的 MAC 地址（6 bytes）
typedef void (*psu_pair_done_cb_t)(const uint8_t peer_mac[6]);

esp_err_t    psu_driver_init        (void);
void         psu_driver_poll        (void);   // 非阻塞，由 task_hal_poll 每 tick（10 ms）呼叫
void         psu_driver_set_voltage (float v);
void         psu_driver_set_current (float a);
psu_status_t psu_driver_get_status  (void);

// 節點是否接受電壓／電流設定。還不知道節點能力（尚未收到 CAP）時回傳 true，
// 讓 SET 照送 —— 不支援的節點會回 ACK UNSUPPORTED，不會有副作用。
bool psu_driver_can_set_voltage(void);
bool psu_driver_can_set_current(void);

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
