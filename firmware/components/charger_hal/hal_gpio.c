#include "hal/hal_gpio.h"
#include <driver/gpio.h>

// 每個輸出最後寫入的電位，供 hal_gpio_outputs_get() 回報。各自一個 bool 而不是
// 共用一個位元遮罩：繼電器由 task_tes_sm 寫、LED 由 task_display 寫，兩個任務
// 對同一個位元組做「讀-改-寫」會互相蓋掉。單一 bool 的寫入本身是原子的。
static volatile bool s_out[6];
enum { O_RELAY, O_LOCK, O_VP, O_LED_S, O_LED_C, O_LED_E };

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
    for (int i = 0; i < 6; i++) s_out[i] = false;
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
    s_out[O_RELAY] = on;
}

void hal_gpio_coupler_lock_set(bool lock)
{
    gpio_set_level(PIN_SOLENOID_LOCK, lock ? 1 : 0);
    s_out[O_LOCK] = lock;
}

void hal_gpio_vp_relay_set(bool on)
{
    gpio_set_level(PIN_RELAY_VP, on ? 1 : 0);
    s_out[O_VP] = on;
}

bool hal_gpio_relay_get(void)   { return s_out[O_RELAY]; }

void hal_gpio_led_standby_set (bool on) { gpio_set_level(PIN_LED_STANDBY,  on ? 1 : 0); s_out[O_LED_S] = on; }
void hal_gpio_led_charging_set(bool on) { gpio_set_level(PIN_LED_CHARGING, on ? 1 : 0); s_out[O_LED_C] = on; }
void hal_gpio_led_error_set   (bool on) { gpio_set_level(PIN_LED_ERROR,    on ? 1 : 0); s_out[O_LED_E] = on; }

uint8_t hal_gpio_outputs_get(void)
{
    uint8_t m = 0;
    if (s_out[O_RELAY]) m |= HAL_OUT_RELAY;
    if (s_out[O_LOCK])  m |= HAL_OUT_LOCK;
    if (s_out[O_VP])    m |= HAL_OUT_VP;
    if (s_out[O_LED_S]) m |= HAL_OUT_LED_STANDBY;
    if (s_out[O_LED_C]) m |= HAL_OUT_LED_CHARGING;
    if (s_out[O_LED_E]) m |= HAL_OUT_LED_ERROR;
    return m;
}
