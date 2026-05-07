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

static tes_sm_t  s_sm;

// Session energy tracking
static bool     s_session_active        = false;
static float    s_session_energy_wh     = 0.0f;
static uint8_t  s_soc_start             = 0;
static uint32_t s_session_end_sec       = 0;
static bool     s_session_psu_seen      = false; // PSU connected at any tick during session

// CAN diag: last received RX raw values + last sent TX frames
static uint8_t              s_last_5f0_flags = 0;
static tes_charger_status_t s_last_508       = {0};
static tes_charger_params_t s_last_509       = {0};
static uint8_t              s_last_5f8_flags = 0;

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
            s_last_5f0_flags = em.error_request_flags;
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
        inputs.charge_timer_min = cfg->charge_timer_min;

        tes_sm_tick(&s_sm, &inputs, &outputs);
        execute_outputs(&outputs);

        // Cache last sent TX frames for CAN diag
        if (outputs.tx_charger_status) s_last_508       = outputs.status_508;
        if (outputs.tx_charger_params)  s_last_509       = outputs.params_509;
        if (outputs.tx_emergency)       s_last_5f8_flags = outputs.emergency_5f8.emergency_flags;

        inputs.start_requested       = false;
        inputs.stop_requested        = false;
        inputs.fault_clear_requested = false;
        inputs.vehicle_emergency     = false; // 每 tick 重置：只反映本 tick 收到的 0x5F0 幀

        tes_snapshot_t snap = tes_sm_get_snapshot(&s_sm);
        snap.energy_wh     = s_session_active ? s_session_energy_wh : 0.0f;
        snap.psu_connected = psu.connected;
        // CAN diag — RX side
        snap.can.v500_fault        = inputs.vehicle_status.fault_flags;
        snap.can.v500_status       = inputs.vehicle_status.status_flags;
        snap.can.v500_req_current  = inputs.vehicle_status.charge_current_cmd / 10.0f;
        snap.can.v500_req_voltage  = inputs.vehicle_status.charge_voltage_limit / 10.0f;
        snap.can.v500_max_voltage  = inputs.vehicle_status.max_charge_voltage / 10.0f;
        snap.can.v501_seq          = inputs.vehicle_params.seq_num;
        snap.can.v501_max_time     = inputs.vehicle_params.max_charge_time_min;
        snap.can.v501_eta          = inputs.vehicle_params.est_end_time_min;
        snap.can.v5f0_flags        = s_last_5f0_flags;
        // CAN diag — TX side
        snap.can.c508_fault        = s_last_508.fault_flags;
        snap.can.c508_status       = s_last_508.status_flags;
        snap.can.c508_avail_voltage= s_last_508.available_voltage / 10.0f;
        snap.can.c508_avail_current= s_last_508.available_current / 10.0f;
        snap.can.c508_fault_voltage= s_last_508.fault_detect_voltage / 10.0f;
        snap.can.c509_rated_kw     = s_last_509.rated_power_50w;
        snap.can.c509_voltage      = s_last_509.actual_voltage / 10.0f;
        snap.can.c509_current      = s_last_509.actual_current / 10.0f;
        snap.can.c509_remaining    = s_last_509.remaining_time_min;
        snap.can.c5f8_flags        = s_last_5f8_flags;
        if (xSemaphoreTake(g_snapshot_mutex, 0) == pdTRUE) {
            g_snapshot = snap;
            xSemaphoreGive(g_snapshot_mutex);
        }

        // Energy accumulation during CHARGING
        if (snap.state == TES_STATE_CHARGING && s_session_active &&
            snap.output_voltage > 0.0f && snap.output_current > 0.0f) {
            s_session_energy_wh += snap.output_voltage * snap.output_current / 360000.0f;
            if (psu.connected) s_session_psu_seen = true;
        }

        static tes_state_t s_last_state = TES_STATE_IDLE;
        if (snap.state != s_last_state) {
            tes_state_t prev  = s_last_state;
            tes_state_t curr  = snap.state;
            static const char *state_names[] = {
                "IDLE","PARAM_EXCHANGE","PRE_CHARGE","CHARGING",
                "ENDING","FINALIZE","FAULT","EMERGENCY"
            };
            const char *name = (curr < 8) ? state_names[curr] : "?";
            ESP_LOGI(TAG, "state: %s -> %s  psu_conn=%d cp_v=%.2f",
                     state_names[prev], name,
                     inputs.psu_connected, inputs.cp_voltage);
            s_last_state = curr;

            // Session start
            if (curr == TES_STATE_CHARGING && !s_session_active) {
                s_session_active    = true;
                s_session_energy_wh = 0.0f;
                s_soc_start         = snap.soc;
                s_session_psu_seen  = false;
            }
            // Capture duration the tick we leave CHARGING (timer still valid)
            if (prev == TES_STATE_CHARGING) {
                s_session_end_sec = snap.elapsed_seconds;
            }
            // Session end: publish when reaching terminal state
            if (s_session_active && (curr == TES_STATE_IDLE ||
                                     curr == TES_STATE_FAULT ||
                                     curr == TES_STATE_EMERGENCY)) {
                uint8_t reason;
                if      (curr == TES_STATE_FAULT)     reason = STOP_REASON_FAULT;
                else if (curr == TES_STATE_EMERGENCY) reason = STOP_REASON_EMERG;
                else if (snap.charge_complete)        reason = STOP_REASON_NORMAL;
                else                                  reason = STOP_REASON_USER;

                charge_session_t sess = {
                    .duration_s       = s_session_end_sec,
                    .energy_wh        = s_session_energy_wh,
                    .soc_start        = s_soc_start,
                    .soc_end          = snap.soc,
                    .stop_reason      = reason,
                    .energy_estimated = s_session_psu_seen ? 0 : 1,
                };
                charger_event_t sess_evt = { .type = EVT_SESSION_COMPLETE,
                                             .timestamp_ms = inputs.tick_ms };
                memcpy(sess_evt.payload, &sess, sizeof(charge_session_t));
                event_bus_publish(&sess_evt);
                s_session_active = false;
            }

            charger_event_t evt = { .type = EVT_TES_STATE_CHANGED,
                                    .timestamp_ms = inputs.tick_ms };
            evt.payload[0] = (uint8_t)curr;
            event_bus_publish(&evt);
        }
    }
}
