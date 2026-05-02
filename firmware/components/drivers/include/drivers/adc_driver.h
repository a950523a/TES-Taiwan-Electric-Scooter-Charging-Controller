#pragma once
#include <esp_err.h>

// ADS1115 直接操作（替換 Adafruit_ADS1X15 庫）
// I2C 位址：0x48（ADDR pin → GND）
// PGA 設定：±4.096V（GAIN_ONE）
// 模式：單次轉換（由 driver 管理）

// 分壓比（對應 V2 Config.h）
#define ADC_VOLT_R1_KOHM    348.0f   // 量測 120V 的上臂電阻
#define ADC_VOLT_R2_KOHM     12.0f   // 量測 120V 的下臂電阻
#define ADC_CP_R1_OHM       150.0f   // CP 量測上臂
#define ADC_CP_R2_OHM        51.0f   // CP 量測下臂

esp_err_t adc_driver_init(void);

// 差動 AIN0-AIN1：輸出側電壓（透過分壓轉換）
float adc_driver_read_voltage(void);

// 差動 AIN2-AIN3：CP 訊號電壓
float adc_driver_read_cp_voltage(void);
