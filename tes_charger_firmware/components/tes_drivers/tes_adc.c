#include "tes_adc.h"
#include "tes_i2c.h" // 引用 bus_handle
#include "board_bsp.h"
#include "driver/i2c_master.h" // 引用新 API
#include "esp_log.h"
#include <math.h> // for fabs
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ADS1115_ADDR 0x48
#define REG_CONVERSION 0x00
#define REG_CONFIG     0x01

// MUX 配置
#define MUX_DIFF_0_1   0x0000
#define MUX_DIFF_2_3   0x3000

// 分壓參數
#define VOLTAGE_DIVIDER_RATIO_120V  (12.0 / (348.0 + 12.0))
#define VOLTAGE_DIVIDER_RATIO_CP    (51.0 / (150.0 + 51.0))

static const char *TAG = "TES_ADC";

// 這是 ADS1115 的裝置 Handle，初始化後會被賦值
static i2c_master_dev_handle_t adc_dev_handle = NULL;

esp_err_t tes_adc_init(void) {
    // 確保 I2C 總線已經初始化
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C Bus handle is NULL! Call tes_i2c_init() first.");
        return ESP_FAIL;
    }

    // 1. 設定裝置參數
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = 100000,
    };

    // 2. 將裝置掛載到總線上
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &adc_dev_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ADS1115 Device Added successfully");
    } else {
        ESP_LOGE(TAG, "Failed to add ADS1115 device");
    }
    return err;
}

// 內部函數：讀取原始 ADC 值
static int16_t read_ads1115(uint16_t mux) {
    if (adc_dev_handle == NULL) return 0;

    // 1. 拿鎖
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0; // 超時拿不到鎖，放棄
    }

    // 2. I2C 操作 (寫入 Config)
    uint16_t config = 0x8000 | mux | 0x0200 | 0x0100 | 0x0080;
    uint8_t write_buf[3];
    write_buf[0] = REG_CONFIG;
    write_buf[1] = (config >> 8) & 0xFF;
    write_buf[2] = config & 0xFF;

    esp_err_t err = i2c_master_transmit(adc_dev_handle, write_buf, 3, -1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C Transmit Failed");
        xSemaphoreGive(i2c_mutex); // --- 失敗也要釋放鎖！ ---
        return 0;
    }

    vTaskDelay(pdMS_TO_TICKS(10)); 

    // 3. I2C 操作 (指向 Conversion)
    uint8_t reg_ptr = REG_CONVERSION;
    i2c_master_transmit(adc_dev_handle, &reg_ptr, 1, -1);
    
    // 4. I2C 操作 (讀取數據)
    uint8_t read_buf[2];
    i2c_master_receive(adc_dev_handle, read_buf, 2, -1);

    // 5. 釋放鎖 (一定要在 return 之前！)
    xSemaphoreGive(i2c_mutex);

    // 6. 回傳結果
    return (int16_t)((read_buf[0] << 8) | read_buf[1]);
}

// 讀取輸出電壓 (差分 0-1)
float tes_adc_read_voltage_sensor(void) {
    int16_t raw = read_ads1115(MUX_DIFF_0_1);
    // LSB = 4.096V / 32768 = 0.000125V
    float voltage_chip = raw * 0.000125f;
    return fabsf(voltage_chip) / VOLTAGE_DIVIDER_RATIO_120V;
}

// 讀取 CP 電壓 (差分 2-3)
float tes_adc_read_cp_voltage(void) {
    int16_t raw = read_ads1115(MUX_DIFF_2_3);
    float voltage_chip = raw * 0.000125f;
    return fabsf(voltage_chip) / VOLTAGE_DIVIDER_RATIO_CP;
}