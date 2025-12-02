#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


// 初始化 ADC (其實只是檢查 I2C 是否通)
esp_err_t tes_adc_init(void);

// 讀取原始數據 (Raw Value)
// channel: 0-3 代表單端，或者自定義 enum 代表差分
int16_t tes_adc_read_raw(uint8_t mux_config);

// --- 高階封裝 (對應原本 HAL) ---
// 讀取輸出電壓 (差分 0-1)
float tes_adc_read_voltage_sensor(void);

// 讀取 CP 電壓 (差分 2-3)
float tes_adc_read_cp_voltage(void);

#ifdef __cplusplus
}
#endif