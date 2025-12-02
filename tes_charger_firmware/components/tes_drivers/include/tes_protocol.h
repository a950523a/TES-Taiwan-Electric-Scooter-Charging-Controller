#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- 來自車輛的數據 (Rx) ---
typedef struct {
    // === 0x500 (Vehicle Status) ===
    union {
        uint8_t raw;
        struct {
            uint8_t system_error : 1;       // bit 0: 供電系統異常
            uint8_t battery_over_volt : 1;  // bit 1: 電池過壓
            uint8_t battery_under_volt : 1; // bit 2: 電池欠壓
            uint8_t current_mismatch : 1;   // bit 3: 電流差異異常
            uint8_t battery_over_temp : 1;  // bit 4: 電池高溫
            uint8_t voltage_mismatch : 1;   // bit 5: 電壓差異異常
            uint8_t reserved : 2;
        } bits;
    } fault_flags;

    union {
        uint8_t raw;
        struct {
            uint8_t charging_enabled : 1;   // bit 0: 可進行充電 (1=可, 0=不可)
            uint8_t vehicle_status : 1;     // bit 1: 車輛狀態 (0=投入, 1=斷開/完成)
            uint8_t charging_posture : 1;   // bit 2: 車輛姿勢 (0=可充, 1=不可充)
            uint8_t stop_request : 1;       // bit 3: 正常停止請求 (1=要求停止)
            uint8_t reserved : 4;
        } bits;
    } status_flags;

    uint16_t charge_current_request_100mA;  // 充電電流命令 (0.1A)
    uint16_t charge_voltage_limit_100mV;    // 充電電壓上限 (0.1V)
    uint16_t max_charge_voltage_100mV;      // 最大充電電壓 (0.1V)

    // === 0x501 (Vehicle Params) ===
    uint8_t charge_sequence_number;         // 序列號 (Ver)
    uint8_t soc;                            // 充電率 %
    uint16_t max_charge_time_min;           // 最大充電時間 (分)
    uint16_t estimated_end_time_min;        // 預估結束時間 (分)

    // === 0x5F0 (Vehicle Emergency) ===
    uint8_t emergency_stop_request : 1;     // bit 0: 緊急停止要求
    uint8_t welding_detection_error : 1;    // bit 1: 熔接異常
    
    uint16_t max_current_capacity_100mA;    // 本次充電最大電流 (0.1A)

} tes_vehicle_data_t;


// --- 充電樁要發送的數據 (Tx) ---
typedef struct {
    // === 0x508 (Charger Status) ===
    union {
        uint8_t raw;
        struct {
            uint8_t system_error : 1;       // bit 0: 供電系統異常 (逾時等)
            uint8_t internal_error : 1;     // bit 1: 充電樁內部異常
            uint8_t battery_incompatible : 1; // bit 2: 電池不適合
            uint8_t reserved : 5;
        } bits;
    } fault_flags;

    union {
        uint8_t raw;
        struct {
            uint8_t stop_control : 1;       // bit 0: 停止控制 (0=輸出中, 1=停止狀態)
            uint8_t operating_status : 1;   // bit 1: 運作狀態 (0=待機, 1=充電中)
            uint8_t connector_lock : 1;     // bit 2: 電磁鎖 (0=解鎖, 1=閉鎖)
            uint8_t reserved : 5;
        } bits;
    } status_flags;

    uint16_t output_voltage_100mV;          // 可用輸出電壓 (0.1V)
    uint16_t output_current_100mA;          // 可用輸出電流 (0.1A)
    uint16_t error_voltage_limit_100mV;     // 異常判定電壓上限 (0.1V)

    // === 0x509 (Charger Params) ===
    uint8_t sequence_number;                // 序列號 (通常 0x12)
    uint8_t rated_power_50W;                // 額定功率 (50W 單位)
    uint16_t present_voltage_100mV;         // 當前量測電壓
    uint16_t present_current_100mA;         // 當前量測電流
    uint16_t remaining_time_min;            // 剩餘時間 (分)

    // === 0x5F8 (Charger Emergency) ===
    uint8_t emergency_stop_status : 1;      // bit 0: 緊急停止狀態
    uint16_t manufacturer_id;               // 製造商 ID

} tes_charger_data_t;


// --- 全域變數 ---
extern tes_vehicle_data_t tes_vehicle;
extern tes_charger_data_t tes_charger;

// --- 函數介面 ---
void tes_protocol_init(void);

#ifdef __cplusplus
}
#endif