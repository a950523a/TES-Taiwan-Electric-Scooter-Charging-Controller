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

#define DEBOUNCE_COUNT 3

typedef struct {
    bool raw_prev;
    int  count;
    bool stable;
} debounce_t;

static debounce_t s_db[4];   // START, STOP, EMERGENCY, SETTING

static bool debounce_update(debounce_t *db, bool raw)
{
    if (raw != db->raw_prev) {
        db->count    = 0;
        db->raw_prev = raw;
    } else if (db->count < DEBOUNCE_COUNT) {
        db->count++;
        if (db->count == DEBOUNCE_COUNT) {
            db->stable = raw;
            return true;   // stable edge detected
        }
    }
    return false;
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

        // EMERGENCY: always → atomic_bool, never gated by menu
        if (debounce_update(&s_db[2], raw_emergency) && s_db[2].stable) {
            atomic_store(&g_emergency_stop, s_db[2].stable);
        }

        // START / STOP: route to TES SM or display depending on menu state
        if (debounce_update(&s_db[0], raw_start) && s_db[0].stable) {
            uint8_t evt = EVT_BUTTON_START;
            if (g_menu_open)
                xQueueSendToBack(g_display_btn_queue, &evt, 0);
            else
                xQueueSendToBack(g_btn_event_queue,   &evt, 0);
        }
        if (debounce_update(&s_db[1], raw_stop) && s_db[1].stable) {
            uint8_t evt = EVT_BUTTON_STOP;
            if (g_menu_open)
                xQueueSendToBack(g_display_btn_queue, &evt, 0);
            else
                xQueueSendToBack(g_btn_event_queue,   &evt, 0);
        }

        // SETTING: always → display queue (opens menu or navigates within it)
        if (debounce_update(&s_db[3], raw_setting) && s_db[3].stable) {
            uint8_t evt = EVT_BUTTON_SETTING;
            xQueueSendToBack(g_display_btn_queue, &evt, 0);
        }

        // ADC: interleave across ticks so 9ms conversion doesn't blow 10ms budget
        adc_divider++;
        if (adc_divider == 5) {
            g_adc_cp_voltage = adc_driver_read_cp_voltage();
        } else if (adc_divider >= 10) {
            g_adc_output_voltage = adc_driver_read_voltage();
            adc_divider = 0;
        }
    }
}
