#pragma once
// tes_sm.h — TES-0D-02-01 狀態機（純函數式，零平台依賴）
//
// 設計原則：
//   tes_sm_tick() 只讀 inputs，只寫 outputs，不呼叫任何 HAL/Driver/OS API。
//   時序由 caller 提供 tick_ms，硬體操作由 task_tes_sm.c 在 tick 後執行。
//   這樣狀態機可以在 PC 上以純 C 單元測試。
#include "tes_protocol/tes_types.h"
#include <stdint.h>
#include <stdbool.h>

// ─── 狀態機輸入（由 task_tes_sm 在每次 tick 前組裝）──────────────────────────

typedef struct {
    uint32_t tick_ms;               // 當前時間（ms），由 platform_tick_ms() 提供

    // 來自 CAN 解碼（最新一次收到的車端資料）
    tes_vehicle_status_t    vehicle_status;
    tes_vehicle_params_t    vehicle_params;
    bool                    vehicle_emergency;  // H'5F0 bit0

    // 來自 ADC / PSU（由 hal_poll_task 更新後傳入）
    float    cp_voltage;            // V，ADS1115 量測
    float    measured_voltage;      // V，輸出側電壓（繼電器閉合後）
    bool     psu_connected;         // PSU UART 連線狀態
    float    psu_voltage;           // V，PSU 回報
    float    psu_current;           // A，PSU 回報

    // 來自 config_svc（每次 tick 更新）
    uint16_t    max_voltage_01v;
    uint16_t    max_current_01a;
    int8_t      target_soc;
    stop_mode_t stop_mode;          // 充電停止條件
    uint16_t    stop_voltage_01v;   // 停止電壓（stop_mode=VOLTAGE 時有效）
    uint16_t    charge_timer_min;   // 充電時長上限（stop_mode=TIMER 時有效，分鐘）

    // 來自按鈕 / remote（atomic flags，讀後清除）
    bool start_requested;
    bool stop_requested;
    bool emergency_requested;       // 硬體緊急按鈕，不經 queue
    bool fault_clear_requested;     // 手動復歸緊急停止（設定選單觸發）
} tes_sm_inputs_t;

// ─── 狀態機輸出（由 task_tes_sm 在 tick 後執行）──────────────────────────────

typedef struct {
    // 硬體控制
    bool  relay_on;
    bool  coupler_lock;
    bool  vp_relay;

    // PSU 指令（僅在 set_psu_* 為 true 時有效）
    bool  set_psu_voltage;
    float psu_voltage_target;       // V
    bool  set_psu_current;
    float psu_current_target;       // A

    // CAN 發送請求（task 讀到 true 後立刻發送）
    bool tx_charger_status;         // H'508
    bool tx_charger_params;         // H'509
    bool tx_emergency;              // H'5F8

    tes_charger_status_t    status_508;
    tes_charger_params_t    params_509;
    tes_charger_emergency_t emergency_5f8;
} tes_sm_outputs_t;

// ─── 狀態機內部狀態（concrete struct，靜態分配，不需 heap）───────────────────

typedef struct {
    tes_state_t          state;
    tes_sm_config_t      cfg;

    // 對外發送的值（維護中間狀態以計算 delta）
    tes_charger_status_t status_508;
    tes_charger_params_t params_509;

    // 流程控制旗標
    bool             vehicle_ready;
    bool             fault_latched;
    bool             charge_complete_latched;

    // 預充電子步驟
    precharge_step_t precharge_step;
    uint32_t         relay_delay_start_ms;

    // 計時器
    uint32_t state_start_ms;
    uint32_t last_periodic_ms;
    uint32_t last_cp_read_ms;

    bool     timer_running;
    uint32_t elapsed_seconds;
    uint32_t total_seconds;
    uint32_t last_timer_ms;

    // CP 狀態（狀態機內部去抖計數）
    cp_state_t cp_state;
    uint8_t    cp_error_count;

    // 快照用 latch 數據
    float    last_valid_req_current;
    uint16_t last_vehicle_voltage_01v;
    uint16_t live_voltage_01v;
    uint16_t live_current_01a;
    uint8_t  last_fault_flags;
    uint8_t  soc;

    // remote control flags（由 network / web 設定，tick 內讀取後清除）
    bool remote_start;
    bool remote_stop;
    bool remote_fault_clear;

    // 緊急停止來源追蹤
    bool emergency_hw_triggered;   // true=硬體按鈕，false=車端 0x5F0
} tes_sm_t;

// ─── 公開 API ────────────────────────────────────────────────────────────────

void           tes_sm_init        (tes_sm_t *sm, const tes_sm_config_t *cfg);
void           tes_sm_tick        (tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
tes_snapshot_t tes_sm_get_snapshot(const tes_sm_t *sm);

// 遠端控制（由 network_svc / display_svc 呼叫，thread-safe by atomic flag）
void tes_sm_request_start       (tes_sm_t *sm);
void tes_sm_request_stop        (tes_sm_t *sm);
void tes_sm_request_fault_clear (tes_sm_t *sm);
