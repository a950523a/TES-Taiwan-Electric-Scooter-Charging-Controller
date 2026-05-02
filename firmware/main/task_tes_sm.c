#include "globals.h"
#include "tes_protocol/tes_sm.h"
#include "tes_protocol/tes_codec.h"
#include "tes_protocol/tes_types.h"
#include "drivers/can_driver.h"
#include "drivers/psu_driver.h"
#include "hal/hal_gpio.h"
#include "services/config_svc.h"
#include "services/event_bus.h"
#include "platform/platform.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "task_tes_sm";

#define TICK_MS 10

static tes_sm_t s_sm;

static void drain_can_rx_queue(tes_sm_inputs_t *in)
{
    can_frame_t frame;
    while (xQueueReceive(g_can_rx_queue, &frame, 0) == pdTRUE) {
        switch (frame.id) {
        case 0x500:
            tes_codec_decode_vehicle_status(frame.data, frame.dlc, &in->vehicle_status);
            break;
        case 0x501:
            tes_codec_decode_vehicle_params(frame.data, frame.dlc, &in->vehicle_params);
            break;
        case 0x5F0: {
            tes_vehicle_emergency_t em;
            tes_codec_decode_vehicle_emergency(frame.data, frame.dlc, &em);
            in->vehicle_emergency = (em.error_request_flags & 0x01) != 0;
            break;
        }
        default: break;
        }
    }
}

static void execute_outputs(const tes_sm_outputs_t *out)
{
    hal_gpio_relay_set(out->relay_on);
    hal_gpio_coupler_lock_set(out->coupler_lock);
    hal_gpio_vp_relay_set(out->vp_relay);

    if (out->set_psu_voltage) psu_driver_set_voltage(out->psu_voltage_target);
    if (out->set_psu_current) psu_driver_set_current(out->psu_current_target);

    if (out->tx_charger_status) {
        can_frame_t f = { .id = 0x508, .dlc = 8 };
        tes_codec_encode_charger_status(&out->status_508, f.data);
        can_driver_send(&f, 0);
    }
    if (out->tx_charger_params) {
        can_frame_t f = { .id = 0x509, .dlc = 8 };
        tes_codec_encode_charger_params(&out->params_509, f.data);
        can_driver_send(&f, 0);
    }
    if (out->tx_emergency) {
        can_frame_t f = { .id = 0x5F8, .dlc = 8 };
        tes_codec_encode_charger_emergency(&out->emergency_5f8, f.data);
        can_driver_send(&f, 0);
    }
}

void task_tes_sm(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "started");

    const charger_config_t *cfg = config_svc_get();
    tes_sm_config_t sm_cfg = {
        .max_voltage_01v   = cfg->max_voltage_01v,
        .max_current_01a   = cfg->max_current_01a,
        .target_soc        = cfg->target_soc,
        .manufacturer_code = 0x0001,
    };
    tes_sm_init(&s_sm, &sm_cfg);

    tes_sm_inputs_t  inputs;
    tes_sm_outputs_t outputs;
    memset(&inputs, 0, sizeof(inputs));

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));

        inputs.tick_ms             = platform_tick_ms();
        inputs.emergency_requested = atomic_load(&g_emergency_stop);

        uint8_t btn_evt;
        while (xQueueReceive(g_btn_event_queue, &btn_evt, 0) == pdTRUE) {
            if (btn_evt == EVT_BUTTON_START) {
                inputs.start_requested = true;
                ESP_LOGI(TAG, "START button received");
            } else if (btn_evt == EVT_BUTTON_STOP) {
                inputs.stop_requested = true;
                ESP_LOGI(TAG, "STOP button received");
            } else if (btn_evt == EVT_FAULT_CLEAR) {
                inputs.fault_clear_requested = true;
                atomic_store(&g_emergency_stop, false); // 解除硬體緊急鎖存
                ESP_LOGI(TAG, "FAULT_CLEAR received");
            }
        }

        drain_can_rx_queue(&inputs);

        inputs.cp_voltage       = g_adc_cp_voltage;
        inputs.measured_voltage = g_adc_output_voltage;

        psu_status_t psu = psu_driver_get_status();
        inputs.psu_connected = psu.connected;
        inputs.psu_voltage   = psu.voltage;
        inputs.psu_current   = psu.current;

        inputs.max_voltage_01v  = cfg->max_voltage_01v;
        inputs.max_current_01a  = cfg->max_current_01a;
        inputs.target_soc       = cfg->target_soc;
        inputs.stop_mode        = cfg->stop_mode;
        inputs.stop_voltage_01v = cfg->stop_voltage_01v;

        tes_sm_tick(&s_sm, &inputs, &outputs);
        execute_outputs(&outputs);

        inputs.start_requested       = false;
        inputs.stop_requested        = false;
        inputs.fault_clear_requested = false;
        inputs.vehicle_emergency     = false; // 每 tick 重置：只反映本 tick 收到的 0x5F0 幀

        tes_snapshot_t snap = tes_sm_get_snapshot(&s_sm);
        if (xSemaphoreTake(g_snapshot_mutex, 0) == pdTRUE) {
            g_snapshot = snap;
            xSemaphoreGive(g_snapshot_mutex);
        }

        static tes_state_t s_last_state = TES_STATE_IDLE;
        if (snap.state != s_last_state) {
            static const char *state_names[] = {
                "IDLE","PARAM_EXCHANGE","PRE_CHARGE","CHARGING",
                "ENDING","FINALIZE","FAULT","EMERGENCY"
            };
            const char *name = (snap.state < 8) ? state_names[snap.state] : "?";
            ESP_LOGI(TAG, "state: %s -> %s  psu_conn=%d cp_v=%.2f",
                     state_names[s_last_state], name,
                     inputs.psu_connected, inputs.cp_voltage);
            s_last_state = snap.state;
            charger_event_t evt = { .type = EVT_TES_STATE_CHANGED,
                                    .timestamp_ms = inputs.tick_ms };
            evt.payload[0] = (uint8_t)snap.state;
            event_bus_publish(&evt);
        }
    }
}
