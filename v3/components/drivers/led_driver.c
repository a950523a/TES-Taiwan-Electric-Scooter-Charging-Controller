#include "drivers/led_driver.h"
#include "hal/hal_gpio.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// LuxBeacon: encode SOC as pulse counts on the green (charging) LED.
// Each decimal digit → N short pulses (0 → 10 pulses).
// Digits separated by 300ms gap; sequence separated by 1000ms gap.

#define PULSE_ON_TICKS    4    // 4 × 50ms = 200ms on
#define PULSE_OFF_TICKS   4    // 4 × 50ms = 200ms off
#define DIGIT_GAP_TICKS   8    // 8 × 50ms = 400ms gap between digits
#define SEQ_GAP_TICKS     20   // 20 × 50ms = 1000ms gap between sequences

typedef enum {
    LUX_IDLE,
    LUX_PULSE_ON,
    LUX_PULSE_OFF,
    LUX_DIGIT_GAP,
    LUX_SEQ_GAP,
} lux_phase_t;

static struct {
    led_state_t  state;
    bool         beacon_enabled;
    int          beacon_soc;

    lux_phase_t  lux_phase;
    uint8_t      digits[3];
    int          num_digits;
    int          digit_idx;
    int          pulses_left;
    int          phase_ticks;

    uint32_t     blink_ticks;
    bool         blink_on;
} s;

// Load digits from SOC without touching phase/timing (called mid-run to refresh SOC).
static void lux_load_digits(int soc)
{
    if (soc < 0)   soc = 0;
    if (soc > 100) soc = 100;
    s.num_digits = 0;
    if (soc >= 100) {
        s.digits[s.num_digits++] = 1;
        s.digits[s.num_digits++] = 0;
        s.digits[s.num_digits++] = 0;
    } else if (soc >= 10) {
        s.digits[s.num_digits++] = (uint8_t)(soc / 10);
        s.digits[s.num_digits++] = (uint8_t)(soc % 10);
    } else {
        s.digits[s.num_digits++] = (uint8_t)soc;
    }
    for (int i = 0; i < s.num_digits; i++) {
        if (s.digits[i] == 0) s.digits[i] = 10;
    }
    s.digit_idx   = 0;
    s.pulses_left = s.digits[0];
}

// Full init: load digits and start the inter-sequence gap.
static void lux_build_pattern(int soc)
{
    lux_load_digits(soc);
    s.lux_phase   = LUX_SEQ_GAP;
    s.phase_ticks = SEQ_GAP_TICKS;
}

void led_driver_init(void)
{
    memset(&s, 0, sizeof(s));
    s.state = LED_STATE_STANDBY;
    hal_gpio_led_standby_set(true);
    hal_gpio_led_charging_set(false);
    hal_gpio_led_error_set(false);
}

void led_driver_set_state(led_state_t state)
{
    s.state = state;
    if (state != LED_STATE_CHARGING) {
        s.lux_phase  = LUX_IDLE;
        s.blink_on   = false;
        s.blink_ticks = 0;
    }
}

void led_driver_set_beacon_enable(bool enabled)
{
    bool was_enabled = s.beacon_enabled;
    s.beacon_enabled = enabled;
    // Only init pattern on false→true transition; led_driver_tick handles LUX_IDLE on entry.
    if (enabled && !was_enabled && s.state == LED_STATE_CHARGING) {
        lux_build_pattern(s.beacon_soc);
    }
}

void led_driver_set_beacon_soc(int soc)
{
    s.beacon_soc = soc;
    // Pattern refreshes with new SOC at the start of each sequence in led_driver_tick().
}

void led_driver_tick(void)
{
    hal_gpio_led_standby_set(true);  // orange always on

    switch (s.state) {
    case LED_STATE_STANDBY:
        hal_gpio_led_charging_set(false);
        hal_gpio_led_error_set(false);
        break;

    case LED_STATE_CHARGING:
        hal_gpio_led_error_set(false);
        if (s.beacon_enabled) {
            if (s.lux_phase == LUX_IDLE) {
                lux_build_pattern(s.beacon_soc);
            }
            s.phase_ticks--;
            if (s.phase_ticks <= 0) {
                switch (s.lux_phase) {
                case LUX_SEQ_GAP:
                    lux_load_digits(s.beacon_soc);  // refresh SOC at sequence start
                    s.lux_phase   = LUX_PULSE_ON;
                    s.phase_ticks = PULSE_ON_TICKS;
                    hal_gpio_led_charging_set(true);
                    break;
                case LUX_DIGIT_GAP:
                    s.lux_phase   = LUX_PULSE_ON;
                    s.phase_ticks = PULSE_ON_TICKS;
                    hal_gpio_led_charging_set(true);
                    break;
                case LUX_PULSE_ON:
                    s.lux_phase   = LUX_PULSE_OFF;
                    s.phase_ticks = PULSE_OFF_TICKS;
                    hal_gpio_led_charging_set(false);
                    break;
                case LUX_PULSE_OFF:
                    s.pulses_left--;
                    if (s.pulses_left > 0) {
                        s.lux_phase   = LUX_PULSE_ON;
                        s.phase_ticks = PULSE_ON_TICKS;
                        hal_gpio_led_charging_set(true);
                    } else {
                        s.digit_idx++;
                        if (s.digit_idx < s.num_digits) {
                            s.pulses_left = s.digits[s.digit_idx];
                            s.lux_phase   = LUX_DIGIT_GAP;
                            s.phase_ticks = DIGIT_GAP_TICKS;
                        } else {
                            s.lux_phase   = LUX_SEQ_GAP;
                            s.phase_ticks = SEQ_GAP_TICKS;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        } else {
            s.blink_ticks++;
            if (s.blink_ticks >= 10) {
                s.blink_ticks = 0;
                s.blink_on    = !s.blink_on;
            }
            hal_gpio_led_charging_set(s.blink_on);
        }
        break;

    case LED_STATE_COMPLETE:
        hal_gpio_led_charging_set(true);
        hal_gpio_led_error_set(false);
        break;

    case LED_STATE_FAULT:
        hal_gpio_led_charging_set(false);
        hal_gpio_led_error_set(true);
        break;
    }
}
