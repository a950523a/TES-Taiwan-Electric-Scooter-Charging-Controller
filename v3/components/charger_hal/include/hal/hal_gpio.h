#pragma once
#include <stdbool.h>
#include <driver/gpio.h>

// GPIO 腳位配置（對應 V2 Config.h）
#define PIN_BTN_START       GPIO_NUM_42
#define PIN_BTN_STOP        GPIO_NUM_39
#define PIN_BTN_EMERGENCY   GPIO_NUM_40
#define PIN_BTN_SETTING     GPIO_NUM_41

#define PIN_LED_STANDBY     GPIO_NUM_5   // 橘燈
#define PIN_LED_CHARGING    GPIO_NUM_6   // 綠燈
#define PIN_LED_ERROR       GPIO_NUM_7   // 紅燈

#define PIN_RELAY_CHARGE    GPIO_NUM_11  // 主充電繼電器
#define PIN_SOLENOID_LOCK   GPIO_NUM_10  // 電磁鎖
#define PIN_RELAY_VP        GPIO_NUM_9   // VP 繼電器

typedef enum {
    BTN_START = 0,
    BTN_STOP,
    BTN_EMERGENCY,
    BTN_SETTING,
} button_id_t;

void hal_gpio_init(void);

// 按鈕：active-low，debounce 由 task_hal_poll 處理
bool hal_gpio_button_raw(button_id_t btn);

// 繼電器與輸出控制
void hal_gpio_relay_set       (bool on);
void hal_gpio_coupler_lock_set(bool lock);
void hal_gpio_vp_relay_set    (bool on);
bool hal_gpio_relay_get       (void);

// LED（直接 GPIO，pattern 由 led_driver 管理）
void hal_gpio_led_standby_set (bool on);
void hal_gpio_led_charging_set(bool on);
void hal_gpio_led_error_set   (bool on);
