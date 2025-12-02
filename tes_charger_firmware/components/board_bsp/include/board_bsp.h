#pragma once
#include "board_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


// --- 硬體定義 (在 board_v1.c 實作) ---
extern const tes_io_t PIN_LED_STATUS;
extern const tes_can_pins_t BOARD_CAN_PINS;

// --- 控制函數 (在 board_bsp.c 實作) ---
// 初始化 IO
esp_err_t tes_io_init(const tes_io_t *io, bool is_output);

// 設定電位 (1 = ON, 0 = OFF) - 會自動處理 inverted
esp_err_t tes_io_set_level(const tes_io_t *io, int level);

// 翻轉電位
esp_err_t tes_io_toggle(const tes_io_t *io);

// --- 新增硬體宣告 ---
extern const tes_io_t PIN_RELAY_MAIN;
extern const tes_io_t PIN_RELAY_VP;
extern const tes_io_t PIN_RELAY_LOCK;

extern const tes_io_t PIN_BTN_START;
extern const tes_io_t PIN_BTN_SETTING;
extern const tes_io_t PIN_BTN_STOP;
extern const tes_io_t PIN_BTN_EMERGENCY;

// --- 新增讀取函數 ---
// 讀取 IO 電位 (回傳 0 或 1，會自動處理 inverted)
int tes_io_get_level(const tes_io_t *io);

extern const tes_i2c_config_t BOARD_I2C_MAIN;

#ifdef __cplusplus
}
#endif