#include "tes_i2c.h" // 為了取得 bus_handle
#include "u8g2.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "U8G2_HAL";

// I2C Device Handle
static i2c_master_dev_handle_t oled_dev_handle = NULL;

// I2C 地址 (通常是 0x3C 或 0x3D)
#define OLED_I2C_ADDRESS 0x3C

// --- 初始化函數 (會被 byte_cb 呼叫) ---
static void tes_u8g2_i2c_init(void) {
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C Bus not initialized!");
        return;
    }
    
    if (oled_dev_handle != NULL) {
        return; // 已經初始化過了
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = 400000, // OLED 可以跑 400kHz
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &oled_dev_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OLED Device Added");
    } else {
        ESP_LOGE(TAG, "Failed to add OLED device");
    }
}

// --- U8g2 I2C 通訊回調函數 ---
uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buffer[32];
    static uint8_t buf_idx = 0;

    switch(msg) {
        case U8X8_MSG_BYTE_SEND: {
            uint8_t *data = (uint8_t *)arg_ptr;
            while(arg_int > 0) {
                if (buf_idx < sizeof(buffer)) {
                    buffer[buf_idx++] = *data;
                }
                data++;
                arg_int--;
            }
            break;
        }
        
        case U8X8_MSG_BYTE_INIT:
            tes_u8g2_i2c_init();
            break;
            
        case U8X8_MSG_BYTE_SET_DC:
            // I2C OLED 不需要 DC 腳位控制
            break;
            
        case U8X8_MSG_BYTE_START_TRANSFER:
            buf_idx = 0;
            break;
            
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (oled_dev_handle && buf_idx > 0) {
                // 使用 ESP-IDF v5 API 發送
                // 注意：U8g2 已經在 buffer[0] 放了 Control Byte，所以我們直接發送整個 buffer
                i2c_master_transmit(oled_dev_handle, buffer, buf_idx, -1);
            }
            break;
            
        default:
            return 0;
    }
    return 1;
}

// --- U8g2 GPIO & Delay 回調函數 ---
uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch(msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            // 如果有 Reset 腳位，在這裡初始化 GPIO
            break;

        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;

        case U8X8_MSG_DELAY_10MICRO:
            // 微秒級延遲，通常用空迴圈或 ets_delay_us
            esp_rom_delay_us(arg_int * 10);
            break;

        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1); // 最小單位給 1us 已經夠快了
            break;

        case U8X8_MSG_GPIO_RESET:
            // 如果有 Reset 腳位，在這裡控制
            // gpio_set_level(RESET_PIN, arg_int);
            break;

        default:
            return 0;
    }
    return 1;
}