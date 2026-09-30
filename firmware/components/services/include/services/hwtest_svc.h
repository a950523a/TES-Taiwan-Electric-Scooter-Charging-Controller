#pragma once
#include <stdbool.h>
#include <stdint.h>

// 工作台測試模式：從硬體狀態頁（/hw）手動切換繼電器、電子鎖、VP 與 LED，用來
// 驗板子。這支只管「要求」和「租約」；**能不能接管由 task_tes_sm 每 tick 判斷**，
// 判斷結果（hwtest_block_t）餵給 hwtest_svc_tick()。條件寫在狀態機旁邊，是因為
// 它們讀的正是狀態機的輸入（狀態、CP、CAN、緊急停止）。
//
// 設計上的三條規則：
//   1. 任何一個阻擋條件成立，同一個 tick 就結束接管，輸出交回狀態機。
//   2. 被條件結束之後不會自動恢復 —— 使用者必須重新進入。否則「接車 → 結束 →
//      VP 關掉 → CP 掉下去 → 又接管」會自己來回跳。
//   3. 租約 HWTEST_LEASE_MS 內沒有續約就結束：頁面關掉、手機鎖屏、WiFi 斷線，
//      輸出都會自己回到狀態機手上。

#define HWTEST_LEASE_MS  10000u

// 測試可以控制的輸出（位元遮罩）
#define HWTEST_OUT_RELAY         0x01   // DC 主繼電器 —— 槍頭端子會帶 PSU 電壓
#define HWTEST_OUT_LOCK          0x02   // 電子鎖
#define HWTEST_OUT_VP            0x04   // VP 繼電器
#define HWTEST_OUT_LED_STANDBY   0x08
#define HWTEST_OUT_LED_CHARGING  0x10
#define HWTEST_OUT_LED_ERROR     0x20
#define HWTEST_OUT_ALL           0x3F

// 不能接管的原因；也用來記錄上一次接管為什麼結束。
typedef enum {
    HWTEST_OK = 0,
    HWTEST_BLOCK_NOT_IDLE,      // 狀態機不在 IDLE（充電流程進行中、故障、緊急）
    HWTEST_BLOCK_EMERGENCY,     // 緊急停止鎖存中
    HWTEST_BLOCK_CP_PRESENT,    // CP 有電壓 —— 有車接著
    HWTEST_BLOCK_VEHICLE_CAN,   // 最近收到車端 0x500 —— 有車接著
    HWTEST_END_LEASE,           // 租約逾時（頁面關了或斷線）
    HWTEST_END_USER,            // 使用者自己結束
    HWTEST_END_REPLACED,        // 被另一個頁面的新接管取代
} hwtest_block_t;

typedef struct {
    bool           active;
    uint8_t        mask;          // 目前要求的輸出（active 時有效）
    uint32_t       lease_left_ms; // active 時租約剩餘
    hwtest_block_t block;         // 現在能不能進入（HWTEST_OK = 可以）
    hwtest_block_t last_end;      // 上一次接管為什麼結束；從未接管為 HWTEST_OK
    uint32_t       owner;         // 目前接管者的代號（頁面產生的亂數）
} hwtest_status_t;

// ── HTTP 端（network_svc）────────────────────────────────────────────────────
// owner 是頁面自己產生的亂數：同時開兩個分頁時，後進入的取代先前的，
// 先前那頁的續約會被拒絕，而不是兩頁輪流改輸出。
hwtest_block_t hwtest_svc_start    (uint32_t owner, uint32_t now_ms);
bool           hwtest_svc_set      (uint32_t owner, uint8_t mask, uint32_t now_ms);  // 同時續約
bool           hwtest_svc_keepalive(uint32_t owner, uint32_t now_ms);
void           hwtest_svc_stop     (uint32_t owner);
hwtest_status_t hwtest_svc_status  (uint32_t now_ms);

// ── task_tes_sm 端，每 tick 一次 ─────────────────────────────────────────────
// block = 本 tick 的阻擋條件。回傳 true 表示接管中，*mask 為要寫到腳位的輸出。
bool hwtest_svc_tick(hwtest_block_t block, uint32_t now_ms, uint8_t *mask);

const char *hwtest_block_name(hwtest_block_t b);
