#include "hal/hal_gpio.h"
#include <driver/gpio.h>

static bool s_relay_state = false;

void hal_gpio_init(void)
{
    // 按鈕：INPUT_PULLUP，active-low
    const gpio_num_t buttons[] = {
        PIN_BTN_START, PIN_BTN_STOP, PIN_BTN_EMERGENCY, PIN_BTN_SETTING
    };
    for (int i = 0; i < 4; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << buttons[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }

    // 輸出：繼電器、電磁鎖、VP、LED（預設低電位）
    const gpio_num_t outputs[] = {
        PIN_RELAY_CHARGE, PIN_SOLENOID_LOCK, PIN_RELAY_VP,
        PIN_LED_STANDBY, PIN_LED_CHARGING, PIN_LED_ERROR
    };
    for (int i = 0; i < 6; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << outputs[i],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(outputs[i], 0);
    }
    s_relay_state = false;
}

bool hal_gpio_button_raw(button_id_t btn)
{
    gpio_num_t pin;
    switch (btn) {
        case BTN_START:     pin = PIN_BTN_START;     break;
        case BTN_STOP:      pin = PIN_BTN_STOP;      break;
        case BTN_EMERGENCY: pin = PIN_BTN_EMERGENCY; break;
        case BTN_SETTING:   pin = PIN_BTN_SETTING;   break;
        default: return false;
    }
    return gpio_get_level(pin) == 0; // active-low
}

void hal_gpio_relay_set(bool on)
{
    gpio_set_level(PIN_RELAY_CHARGE, on ? 1 : 0);
    s_relay_state = on;
}

void hal_gpio_coupler_lock_set(bool lock)
{
    gpio_set_level(PIN_SOLENOID_LOCK, lock ? 1 : 0);
}

void hal_gpio_vp_relay_set(bool on)
{
    gpio_set_level(PIN_RELAY_VP, on ? 1 : 0);
}

bool hal_gpio_relay_get(void)   { return s_relay_state; }

void hal_gpio_led_standby_set (bool on) { gpio_set_level(PIN_LED_STANDBY,  on ? 1 : 0); }
void hal_gpio_led_charging_set(bool on) { gpio_set_level(PIN_LED_CHARGING, on ? 1 : 0); }
void hal_gpio_led_error_set   (bool on) { gpio_set_level(PIN_LED_ERROR,    on ? 1 : 0); }
