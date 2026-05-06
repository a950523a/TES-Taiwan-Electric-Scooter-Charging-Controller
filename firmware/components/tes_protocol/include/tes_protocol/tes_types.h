#pragma once
// tes_types.h — TES-0D-02-01 共用型別
// 零平台依賴：只用 stdint.h / stdbool.h，可在任何 C99 環境編譯
#include <stdint.h>
#include <stdbool.h>

// ─── 狀態機狀態 ───────────────────────────────────────────────────────────────

typedef enum {
    TES_STATE_IDLE = 0,
    TES_STATE_PARAM_EXCHANGE,   // 初始參數交換（等待 CAN 許可）
    TES_STATE_PRE_CHARGE,       // 預充電操作（電磁鎖、絕緣測試、繼電器）
    TES_STATE_CHARGING,         // DC 電流輸出中
    TES_STATE_ENDING,           // 正常結束流程（斷繼電器、等車端）
    TES_STATE_FAULT,            // 故障處理（顯示 10 秒後回 IDLE）
    TES_STATE_EMERGENCY,        // 緊急停止程序
    TES_STATE_FINALIZE,         // 最終化（確認輸出電壓歸零）
} tes_state_t;

// ─── 充電停止模式 ──────────────────────────────────────────────────────────────

typedef enum {
    STOP_MODE_SOC     = 0,   // 依 BMS 回報 SOC 停止（預設）
    STOP_MODE_VOLTAGE = 1,   // 依輸出電壓停止
} stop_mode_t;

// ─── CP 電壓狀態（以 ADS1115 讀取直流電壓判斷） ──────────────────────────────

typedef enum {
    CP_STATE_UNKNOWN = 0,
    CP_STATE_OFF,       // 0.0 – 1.9 V
    CP_STATE_ON,        // 7.4 – 13.7 V
    CP_STATE_ERROR,     // 其他電壓值
} cp_state_t;

// ─── LED 指示燈狀態 ────────────────────────────────────────────────────────────

typedef enum {
    LED_STATE_STANDBY = 0,
    LED_STATE_CHARGING,
    LED_STATE_COMPLETE,
    LED_STATE_FAULT,
} led_state_t;

// ─── 預充電子步驟（供單元測試可見） ──────────────────────────────────────────

typedef enum {
    PRECHARGE_STEP_INIT = 0,
    PRECHARGE_STEP_READY_ANNOUNCED,  // 已發送 0x508 ready 位元
    PRECHARGE_STEP_CONTACTOR_WAIT,   // 等待車端接觸器閉合
    PRECHARGE_STEP_RELAY_DELAY,      // 接觸器閉合後 250ms 延遲
    PRECHARGE_STEP_COMPLETE,         // 充電繼電器已閉合，進入 CHARGING
} precharge_step_t;

// ─── CAN 訊框結構（TES-0D-02-01）────────────────────────────────────────────

// H'500  Vehicle → Charger（狀態與需求）
typedef struct {
    uint8_t  fault_flags;           // byte 0：故障旗標
    uint8_t  status_flags;          // byte 1：bit0=CAN許可 bit1=接觸器開路 bit2=停止請求 bit3=正常停止
    uint16_t charge_current_cmd;    // bytes 2-3：0.1A/bit，BMS 請求電流
    uint16_t charge_voltage_limit;  // bytes 4-5：0.1V/bit，充電電壓上限
    uint16_t max_charge_voltage;    // bytes 6-7：0.1V/bit，最大允許電壓
} tes_vehicle_status_t;             // CAN ID 0x500

// H'501  Vehicle → Charger（參數）
typedef struct {
    uint8_t  seq_num;               // byte 0：esChargeSequenceNumber
    uint8_t  soc;                   // byte 1：1%/bit，電量百分比
    uint16_t max_charge_time_min;   // bytes 2-3：1min/bit，0xFFFF=未知
    uint16_t est_end_time_min;      // bytes 4-5：1min/bit，預估結束時間
} tes_vehicle_params_t;             // CAN ID 0x501

// H'5F0  Vehicle → Charger（緊急）
typedef struct {
    uint8_t error_request_flags;    // byte 0：bit0=緊急停止請求
} tes_vehicle_emergency_t;          // CAN ID 0x5F0

// H'508  Charger → Vehicle（充電樁狀態）
typedef struct {
    uint8_t  fault_flags;           // byte 0：故障旗標
    uint8_t  status_flags;          // byte 1：bit0=待機/準備 bit1=充電中 bit2=電磁鎖鎖定
    uint16_t available_voltage;     // bytes 2-3：VLIM，0.1V/bit
    uint16_t available_current;     // bytes 4-5：I_MX，0.1A/bit
    uint16_t fault_detect_voltage;  // bytes 6-7：VLIM2，0.1V/bit
} tes_charger_status_t;             // CAN ID 0x508

// H'509  Charger → Vehicle（充電樁參數）
typedef struct {
    uint8_t  seq_num;               // byte 0：esChargeSequenceNumber（硬編碼 18）
    uint8_t  rated_power_50w;       // byte 1：50W/bit
    uint16_t actual_voltage;        // bytes 2-3：0.1V/bit，實際輸出電壓
    uint16_t actual_current;        // bytes 4-5：0.1A/bit，實際輸出電流
    uint16_t remaining_time_min;    // bytes 6-7：1min/bit，0xFFFF=未知
} tes_charger_params_t;             // CAN ID 0x509

// H'5F8  Charger → Vehicle（緊急）
typedef struct {
    uint8_t  emergency_flags;       // byte 0：bit0=緊急停止
    uint16_t manufacturer_id;       // bytes 4-5：製造商代碼
} tes_charger_emergency_t;          // CAN ID 0x5F8

// ─── 狀態機配置（初始化時傳入，運行中不變） ──────────────────────────────────

typedef struct {
    uint16_t max_voltage_01v;       // 硬體上限，如 1000 = 100.0V
    uint16_t max_current_01a;       // 硬體上限，如 100 = 10.0A
    int8_t   target_soc;            // 目標電量 0-100%
    uint16_t manufacturer_code;     // H'5F8 製造商代碼
} tes_sm_config_t;

// ─── 充電停止原因 ──────────────────────────────────────────────────────────────

typedef enum {
    STOP_REASON_NORMAL = 0,  // SOC / 電壓目標達成
    STOP_REASON_USER   = 1,  // 手動停止（按鈕或 REST API）
    STOP_REASON_FAULT  = 2,
    STOP_REASON_EMERG  = 3,
    STOP_REASON_BMS    = 4,  // BMS 撤回充電許可
    STOP_REASON_TIMER  = 5,
} stop_reason_t;

// ─── 充電紀錄（可放入 charger_event_t payload，12 bytes） ─────────────────────

typedef struct {
    uint32_t duration_s;    // 充電時長（秒）
    float    energy_wh;     // 充電電量（Wh，V×I × 10ms / 3600000）
    uint8_t  soc_start;     // 充電開始時 SOC（0-100）
    uint8_t  soc_end;       // 充電結束時 SOC（0-100）
    uint8_t  stop_reason;      // stop_reason_t
    uint8_t  energy_estimated; // 1 = PSU 未連線，電量為 ADC 預估值
} charge_session_t;         // 12 bytes

// ─── CAN 診斷快照（最後收到的 0x500 / 0x501 解碼值） ────────────────────────

typedef struct {
    // ── 0x500  Vehicle → Charger ──────────────────────────────────────────
    uint8_t  v500_fault;        // byte 0：故障旗標（0 = 無故障）
    uint8_t  v500_status;       // byte 1：bit0=CAN許可 bit1=接觸器開路 bit2=停止請求 bit3=正常停止
    float    v500_req_current;  // bytes 2-3：BMS 請求電流 (A)
    float    v500_req_voltage;  // bytes 4-5：BMS 充電電壓上限 (V)
    float    v500_max_voltage;  // bytes 6-7：BMS 最大允許電壓 (V)
    // ── 0x501  Vehicle → Charger ──────────────────────────────────────────
    uint8_t  v501_seq;          // byte 0：esChargeSequenceNumber
    uint16_t v501_max_time;     // bytes 2-3：最大充電時間 (min)，0xFFFF=未知
    uint16_t v501_eta;          // bytes 4-5：預估結束時間 (min)，0xFFFF=未知
    // ── 0x5F0  Vehicle → Charger（緊急）────────────────────────────────────
    uint8_t  v5f0_flags;        // byte 0：bit0=緊急停止請求
    // ── 0x508  Charger → Vehicle ──────────────────────────────────────────
    uint8_t  c508_fault;        // byte 0：故障旗標
    uint8_t  c508_status;       // byte 1：bit0=待機/準備 bit1=充電中 bit2=電磁鎖鎖定
    float    c508_avail_voltage;// bytes 2-3：VLIM，可提供電壓 (V)
    float    c508_avail_current;// bytes 4-5：I_MX，可提供電流 (A)
    float    c508_fault_voltage;// bytes 6-7：VLIM2，故障偵測電壓 (V)
    // ── 0x509  Charger → Vehicle ──────────────────────────────────────────
    uint8_t  c509_rated_kw;     // byte 1：額定功率（×50 W）
    float    c509_voltage;      // bytes 2-3：實際輸出電壓 (V)
    float    c509_current;      // bytes 4-5：實際輸出電流 (A)
    uint16_t c509_remaining;    // bytes 6-7：剩餘時間 (min)，0xFFFF=未知
    // ── 0x5F8  Charger → Vehicle（緊急）────────────────────────────────────
    uint8_t  c5f8_flags;        // byte 0：bit0=緊急停止
} tes_can_diag_t;

// ─── 快照（只讀，供 display / network / 未來 JS 讀取） ─────────────────────

typedef struct {
    tes_state_t   state;
    bool          fault_latched;
    bool          charge_complete;
    uint8_t       soc;                   // 0-100 %
    float         output_voltage;        // V（PSU 回報或 ADC 量測）
    float         output_current;        // A
    float         vehicle_req_voltage;   // V（BMS 充電電壓上限，0x500 charge_voltage_limit）
    float         vehicle_req_current;   // A（BMS 請求電流，0x500 charge_current_cmd）
    bool          timer_running;
    uint32_t      elapsed_seconds;
    uint32_t      total_seconds;
    uint32_t      remaining_seconds;
    uint16_t      max_voltage_01v;       // 當前設定值
    uint16_t      max_current_01a;
    int8_t        target_soc;
    led_state_t   led_state;
    uint8_t       last_fault_flags;      // 最後一次故障旗標（用於顯示）
    float         last_valid_req_current;
    float         energy_wh;            // 本次充電累積電量（充電中有效，其他狀態為 0）
    bool          psu_connected;        // PSU UART 已回報（false = 數值為預估）
    tes_can_diag_t can;                 // 最後收到的 CAN 解碼值（診斷用）
} tes_snapshot_t;
