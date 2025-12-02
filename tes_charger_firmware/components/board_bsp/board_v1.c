#include "board_types.h"

// 定義板子上的 LED
const tes_io_t PIN_LED_STANDBY =  { 
    .type=IO_TYPE_INTERNAL_GPIO, 
    .gpio_num=5, .inverted=false, 
    .pull_mode=IO_PULL_NONE 
};

const tes_io_t PIN_LED_CHARGING = { 
    .type=IO_TYPE_INTERNAL_GPIO, 
    .gpio_num=6, 
    .inverted=false, 
    .pull_mode=IO_PULL_NONE 
};

const tes_io_t PIN_LED_ERROR =    { 
    .type=IO_TYPE_INTERNAL_GPIO, 
    .gpio_num=7, 
    .inverted=false, 
    .pull_mode=IO_PULL_NONE 
};

const tes_can_pins_t BOARD_CAN_PINS = {
    .tx_io = 17,
    .rx_io = 18
};

const tes_i2c_config_t BOARD_I2C_MAIN = {
    .sda_io = 16,
    .scl_io = 15,
    .port_num = 0  // 使用 I2C_NUM_0
};

// --- 繼電器 (Active High) ---
const tes_io_t PIN_RELAY_MAIN = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 11,
    .inverted = false,
    .pull_mode = IO_PULL_NONE
};

const tes_io_t PIN_RELAY_VP = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 9,
    .inverted = false,
    .pull_mode = IO_PULL_NONE
};

const tes_io_t PIN_RELAY_LOCK = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 10,
    .inverted = false,
    .pull_mode = IO_PULL_NONE
};

// --- 按鈕 (Active Low) ---
const tes_io_t PIN_BTN_START = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 42,
    .inverted = true,  // 按下接地，所以讀到 0 是 True
    .pull_mode = IO_PULL_UP
};

const tes_io_t PIN_BTN_SETTING = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 41,
    .inverted = true,  // 按下接地，所以讀到 0 是 True
    .pull_mode = IO_PULL_UP
};

const tes_io_t PIN_BTN_STOP = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 39,
    .inverted = true,
    .pull_mode = IO_PULL_UP
};

const tes_io_t PIN_BTN_EMERGENCY = {
    .type = IO_TYPE_INTERNAL_GPIO,
    .gpio_num = 40,
    .inverted = true,
    .pull_mode = IO_PULL_UP
};

const tes_oled_config_t BOARD_OLED_CONFIG = {
    .i2c_address = 0x3C,
    .rotation = OLED_ROTATION_180, // <--- 這裡設定！
    .external_vcc = false
};