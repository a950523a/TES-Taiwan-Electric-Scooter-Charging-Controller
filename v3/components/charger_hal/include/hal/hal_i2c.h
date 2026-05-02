#pragma once
#include <esp_err.h>
#include <driver/i2c.h>
#include <stdint.h>
#include <stddef.h>

#define HAL_I2C_PORT    I2C_NUM_0
#define PIN_I2C_SDA     GPIO_NUM_16
#define PIN_I2C_SCL     GPIO_NUM_15
#define HAL_I2C_FREQ_HZ 400000

esp_err_t hal_i2c_init(void);

// 低階 I2C 讀寫（供 adc_driver 和 display_driver 使用）
esp_err_t hal_i2c_write     (uint8_t addr7, const uint8_t *data, size_t len);
esp_err_t hal_i2c_read      (uint8_t addr7, uint8_t *data, size_t len);
esp_err_t hal_i2c_write_read(uint8_t addr7,
                              const uint8_t *wr, size_t wr_len,
                              uint8_t       *rd, size_t rd_len);
