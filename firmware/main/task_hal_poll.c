#include "globals.h"
#include "hal/hal_gpio.h"
#include "drivers/adc_driver.h"
#include "drivers/psu_driver.h"
#include "services/event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdbool.h>

volatile float g_adc_cp_voltage     = 0.f;
volatile float g_adc_output_voltage = 0.f;

// Timing constants (tick = 10 ms)
#define DEBOUNCE_TICKS    3    // 30 ms
#define LONG_PRESS_TICKS  50   // 500 ms
#define REPEAT_TICKS      10   // 100 ms auto-repeat while held

typedef struct {
    bool raw_prev;
    int  debounce;
    bool stable;
    int  hold;
    bool long_fired;
    int  repeat_acc;
} btn_t;

// START(0), STOP(1), EMERGENCY(2), SETTING(3)
static btn_t s_btn[4];

typedef enum { BTN_NONE = 0, BTN_SHORT, BTN_LONG, BTN_REPEAT } btn_evt_t;

static btn_evt_t btn_update(btn_t *b, bool raw)
{
    if (raw != b->raw_prev) {
        b->debounce = 0;
        b->raw_prev = raw;
    } else if (b->debounce < DEBOUNCE_TICKS) {
        if (++b->debounce == DEBOUNCE_TICKS)
            b->stable = raw;
    }

    btn_evt_t ev = BTN_NONE;

    if (b->stable) {
        b->hold++;
        if (!b->long_fired && b->hold >= LONG_PRESS_TICKS) {
            b->long_fired = true;
            b->repeat_acc = 0;
            ev = BTN_LONG;
        } else if (b->long_fired) {
            if (++b->repeat_acc >= REPEAT_TICKS) {
                b->repeat_acc = 0;
                ev = BTN_REPEAT;
            }
        }
    } else {
        if (b->hold > 0 && !b->long_fired)
            ev = BTN_SHORT;
        b->hold       = 0;
        b->long_fired = false;
        b->repeat_acc = 0;
    }

    return ev;
}

static void send_btn(QueueHandle_t q, uint8_t evt)
{
    xQueueSendToBack(q, &evt, 0);
}

void task_hal_poll(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    int adc_divider = 0;

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));

        psu_driver_poll();

        bool raw_start     = hal_gpio_button_raw(BTN_START);
        bool raw_stop      = hal_gpio_button_raw(BTN_STOP);
        bool raw_emergency = hal_gpio_button_raw(BTN_EMERGENCY);
        bool raw_setting   = hal_gpio_button_raw(BTN_SETTING);

        btn_evt_t ev_start = btn_update(&s_btn[0], raw_start);
        btn_evt_t ev_stop  = btn_update(&s_btn[1], raw_stop);
        btn_evt_t ev_emerg = btn_update(&s_btn[2], raw_emergency);
        btn_evt_t ev_set   = btn_update(&s_btn[3], raw_setting);

        // EMERGENCY: short press → atomic set (long press also counts)
        if (ev_emerg == BTN_SHORT || ev_emerg == BTN_LONG)
            atomic_store(&g_emergency_stop, true);

        // START: short → SM or menu nav; long/repeat → menu edit coarse adjust only
        if (ev_start == BTN_SHORT) {
            if (g_menu_open)
                send_btn(g_display_btn_queue, EVT_BUTTON_START);
            else
                send_btn(g_btn_event_queue, EVT_BUTTON_START);
        }
        if ((ev_start == BTN_LONG || ev_start == BTN_REPEAT) && g_menu_open)
            send_btn(g_display_btn_queue, EVT_BUTTON_START_LONG);

        // STOP: same pattern as START
        if (ev_stop == BTN_SHORT) {
            if (g_menu_open)
                send_btn(g_display_btn_queue, EVT_BUTTON_STOP);
            else
                send_btn(g_btn_event_queue, EVT_BUTTON_STOP);
        }
        if ((ev_stop == BTN_LONG || ev_stop == BTN_REPEAT) && g_menu_open)
            send_btn(g_display_btn_queue, EVT_BUTTON_STOP_LONG);

        // SETTING: short → quick SOC cycle (status) / confirm (menu)
        //          long  → open settings menu (status only)
        if (ev_set == BTN_SHORT)
            send_btn(g_display_btn_queue, EVT_BUTTON_SETTING);
        if (ev_set == BTN_LONG)
            send_btn(g_display_btn_queue, EVT_BUTTON_SETTING_LONG);

        // ADC: interleave across ticks so 9 ms conversion doesn't blow 10 ms budget
        adc_divider++;
        if (adc_divider == 5) {
            g_adc_cp_voltage = adc_driver_read_cp_voltage();
        } else if (adc_divider >= 10) {
            g_adc_output_voltage = adc_driver_read_voltage();
            adc_divider = 0;
        }
    }
}
