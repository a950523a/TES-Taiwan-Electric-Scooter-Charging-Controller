#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// ADS1115 直接操作（替換 Adafruit_ADS1X15 庫）
// I2C 位址：0x48（ADDR pin → GND）
// PGA 設定：±4.096V（GAIN_ONE）
// 模式：單次轉換（由 driver 管理）

#define ADC_CP_R1_OHM       150.0f   // CP 量測上臂
#define ADC_CP_R2_OHM        51.0f   // CP 量測下臂

// ── 硬體版本辨識（AIN3） ─────────────────────────────────────────────────────
// V1.3 起 AIN3 接 R36（VDD33）／R37（GND）的分壓中點 HW_ID；V1.1／V1.2 的
// AIN3 焊死接地。開機讀一次，決定輸出電壓的分壓係數 —— 同一份韌體要同時服務
// 現場的舊板（30.000）和 V1.3（38.954）。級數表見 tools/changes_v13.py 的 R36。
#define ADC_HW_ID_LEVEL_EXT  12   // AIN3 直接接 VDD33：板上有 EEPROM（本韌體尚未支援）

typedef struct {
    int8_t      level;       // 0 = 舊板（AIN3 接地）、1..11 = 分壓級、ADC_HW_ID_LEVEL_EXT、
                             // -1 = 讀不到或落在級距之間
    float       id_volts;    // AIN3 實際讀到的電壓
    const char *name;        // "V1.1/V1.2"、"V1.3"、"unknown"
    bool        known;       // false = 韌體不認得這塊板，已退回舊板的 30.000
    float       volt_ratio;  // 輸出電壓實際使用的分壓係數
    int8_t      ain3_level;  // AIN3 自己判出來的級數（level 可能被出廠紀錄蓋掉，這個不會）
    bool        from_factory;// true = 版本取自已驗證的出廠紀錄，不是 AIN3
    bool        id_mismatch; // 出廠紀錄說的版本和 AIN3 讀到的不同 —— R36／R37／AIN3 焊點要檢查
    uint16_t    max_volt_01v;// 這塊板量得準的最高電壓（0.1 V）：最大電壓、停止電壓、Auto Volt 的上限
} adc_board_t;

// 已驗證的出廠紀錄（簽章＋MAC 綁定，見 services/factory_svc.h）裡登記的級數。
// 出廠紀錄比 AIN3 可靠（AIN3 會因接地不良而飄），有就以它為準：
// V1.3 被誤判成舊板 = 電壓只讀到 77%，依電壓停止時會充過頭，是危險的方向。
// 要在任何地方用到輸出電壓之前呼叫（main.c 在 Auto Volt 與狀態機之前）。
void adc_driver_apply_factory_level(int level);

// 初始化並辨識硬體版本。ADS1115 不在時回傳錯誤，版本維持舊板預設值。
esp_err_t adc_driver_init(void);

// 開機時辨識出來的硬體版本（adc_driver_init 之前呼叫也有值：舊板預設）
const adc_board_t *adc_driver_board(void);

// 差動 AIN0-AIN1：輸出側電壓（依硬體版本的分壓係數換算）
float adc_driver_read_voltage(void);

// 單端 AIN2：CP 訊號電壓
float adc_driver_read_cp_voltage(void);

// 開機以來 ADS1115 讀取失敗的次數（兩個通道合計）。持續上升代表 I2C 有問題 ——
// 讀值此時沿用上一筆，畫面上看起來會「卡住」而不是歸零。
uint32_t adc_driver_fail_count(void);
