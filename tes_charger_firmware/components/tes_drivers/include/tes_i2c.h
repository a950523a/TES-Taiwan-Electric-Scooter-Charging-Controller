#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// 公開 Bus Handle，讓 ADC 和 OLED 驅動使用
extern i2c_master_bus_handle_t bus_handle;

// 初始化 I2C Master Bus
esp_err_t tes_i2c_init(void);

#ifdef __cplusplus
}
#endif