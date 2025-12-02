#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


// 定義 IO 類型
typedef enum {
    IO_TYPE_INTERNAL_GPIO,    // ESP32 本身的 GPIO
    IO_TYPE_EXPANDER          // 未來擴充用
} io_type_t;

typedef enum {
    IO_PULL_NONE,
    IO_PULL_UP,
    IO_PULL_DOWN
} tes_io_pull_t;

// 增加這個枚舉
typedef enum {
    BUTTON_START,
    BUTTON_SETTING,
    BUTTON_STOP,
    BUTTON_EMERGENCY
} tes_button_id_t;

// 定義一個 "腳位物件"
typedef struct {
    io_type_t type;
    int gpio_num;      // ESP32 GPIO 編號
    bool inverted;     // 是否反相 (例如 Active Low 的 LED)
    tes_io_pull_t pull_mode;
} tes_io_t;

typedef struct {
    int tx_io;
    int rx_io;
} tes_can_pins_t;

typedef struct {
    int sda_io;
    int scl_io;
    int port_num; // I2C 控制器編號 (0 或 1)
} tes_i2c_config_t;

#ifdef __cplusplus
}
#endif