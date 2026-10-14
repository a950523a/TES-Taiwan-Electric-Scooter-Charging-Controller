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

    // ── ESP-NOW 連線驗證（UART 不用；見 psu_link/psu_sess.h）─────────────────
    bool     link_auth;       // 已跟配對的節點完成握手，訊框驗證中
    bool     needs_repair;    // 有舊版配對（只有 MAC、沒有金鑰）—— 必須重新配對
    uint32_t auth_rejects;    // 被擋下的訊框：驗證碼不符、重送、沒帶驗證碼的偽造
} psu_status_t;

// 配對進度，給 OLED 與網頁顯示（值見 psu_link/psu_pair.h）
typedef struct {
    uint8_t  state;           // psu_pair_state_t；PSU_PAIR_IDLE = 沒在配對
    uint8_t  fail;            // psu_pair_fail_t
    uint32_t code;            // CONFIRM / DONE 時有效，顯示成 6 位數
    bool     local_ok;        // 這邊已按確認
    bool     peer_ok;         // PSU 那邊已按確認
    bool     legacy_seen;     // 收到舊版 PSU 的純文字 PSU_HELLO —— PSU 韌體要更新
    uint8_t  peer_mac[6];
    uint32_t age_ms;          // 距離進入目前狀態多久（DONE / FAILED 顯示幾秒後收起來）
} psu_pair_info_t;

typedef enum {
    PSU_TRANSPORT_UART   = 0,   // 有線 UART（預設）
    PSU_TRANSPORT_ESPNOW = 1,   // 無線 ESP-NOW
} psu_transport_t;

// 配對完成時的回呼：peer_mac 為 PSU 的 MAC，ltk 為配對算出的長期金鑰（32 B）。
// 兩者都要存進 NVS；ltk 是秘密，不可出現在任何對外介面。
typedef void (*psu_pair_done_cb_t)(const uint8_t peer_mac[6], const uint8_t ltk[32]);

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
// peer_mac_6 = NULL 表示尚未配對。ltk_32 = NULL 而有 peer_mac 表示舊版配對：
// 沒有金鑰就無法驗證，視同未配對，status.needs_repair 會是 true。
esp_err_t psu_driver_set_transport(psu_transport_t t, const uint8_t *peer_mac_6,
                                   const uint8_t *ltk_32);

// 設定配對完成的持久回呼（會在每次配對成功時呼叫）
void psu_driver_set_pair_callback(psu_pair_done_cb_t cb);

// 開始配對（數字比對，見 psu_link/psu_pair.h）。可從任何任務呼叫 —— 只是
// 送出要求，實際在 psu_driver_poll() 裡啟動。cb 可為 NULL（沿用持久回呼）。
void psu_driver_start_pairing(psu_pair_done_cb_t cb);
bool psu_driver_is_pairing(void);

// 使用者對配對碼的回應：accept = 兩邊數字相同。配對碼出來之前傳 false = 取消。
// 可從任何任務呼叫（網頁、按鈕）。
void psu_driver_pair_user(bool accept);

// 配對進行中：START／STOP 必須當成「確認／取消」，不可送進狀態機
bool psu_driver_pair_wants_buttons(void);

psu_pair_info_t psu_driver_pair_info(void);

// ESP-NOW 專用：是否已有已配對的 peer（transport=UART 時永遠回傳 false）
bool psu_driver_has_peer(void);
