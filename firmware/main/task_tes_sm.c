#include "globals.h"
#include "tes_protocol/tes_sm.h"
#include "tes_protocol/tes_codec.h"
#include "tes_protocol/tes_types.h"
#include "drivers/can_driver.h"
#include "drivers/psu_driver.h"
#include "hal/hal_gpio.h"
#include "services/config_svc.h"
#include "services/event_bus.h"
#include "services/trace_svc.h"
#include "platform/platform.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "task_tes_sm";

#define TICK_MS 10

static tes_sm_t  s_sm;

// Session energy tracking
static bool     s_session_active        = false;
static float    s_session_energy_wh     = 0.0f;
static uint8_t  s_soc_start             = 0;
static uint32_t s_session_end_sec       = 0;
static float    s_session_end_v         = 0.0f; // output voltage the tick we left CHARGING
static bool     s_session_psu_seen      = false; // PSU connected at any tick during session

// CAN diag: last received RX raw values + last sent TX frames
static tes_vehicle_emergency_t s_last_5f0    = {0};
static tes_charger_status_t s_last_508       = {0};
static tes_charger_params_t s_last_509       = {0};
static tes_charger_emergency_t s_last_5f8    = {0};

// 收發活性統計。沒有這些數字就分不出「車端沒送」與「送了但值是 0」。
static uint32_t s_rx_500, s_rx_501, s_rx_5f0;
static uint32_t s_rx_500_ms, s_rx_501_ms, s_rx_5f0_ms;   // 最後一次收到的 tick_ms
static bool     s_rx_500_seen, s_rx_501_seen, s_rx_5f0_seen;
static uint32_t s_tx_508, s_tx_509, s_tx_5f8, s_tx_fail;

#define AGE_NEVER 0xFFFFFFFFu
static uint32_t age_of(bool seen, uint32_t last_ms, uint32_t now_ms)
{
    return seen ? (now_ms - last_ms) : AGE_NEVER;
}

// ── Trace（曲線取樣 + 數值變動 Log）─────────────────────────────────────────────
//
// 變動偵測放在這裡而不是 tes_sm.c —— tes_protocol/tes_sm 必須維持零平台依賴，
// 不能碰 PSRAM、FreeRTOS 或字串格式化。

#define TRACE_SAMPLE_PERIOD_MS  5000u   // 充電中的曲線取樣間隔
#define TRACE_DV_01V            5u      // 輸出電壓變動 ≥0.5V 才記
#define TRACE_DI_01A            5u      // 輸出電流變動 ≥0.5A 才記
#define TRACE_DREQ_01A          5u      // BMS 請求電流變動 ≥0.5A 才記

static uint32_t s_trace_session      = 0;   // 目前 session id（0 = 不在充電流程中）
static uint32_t s_trace_session_t0   = 0;   // session 起點的 tick_ms
static uint32_t s_trace_last_sample  = 0;
static bool     s_trace_prev_valid   = false;

// 上一次已記錄的數值快照
static struct {
    uint8_t  state;
    uint8_t  cp_state;
    uint8_t  fault_source;
    bool     relay, lock, vp;
    bool     psu_connected;
    uint8_t  v500_status, v500_fault;
    uint16_t v500_req_current_01a;
    uint16_t v500_req_voltage_01v;
    uint8_t  c508_status, c508_fault;
    uint8_t  soc;
    uint16_t out_v_01v, out_i_01a;
    float    psu_set_v, psu_set_i;
} s_prev;

static const char *tr_state(uint8_t s)
{
    static const char *n[] = { "IDLE","PARAM_EXCHANGE","PRE_CHARGE","CHARGING",
                              "ENDING","FAULT","EMERGENCY","FINALIZE" };
    return (s < 8) ? n[s] : "?";
}

static const char *tr_cp(uint8_t s)
{
    static const char *n[] = { "UNKNOWN","OFF","ON","ERROR" };
    return (s < 4) ? n[s] : "?";
}

// 只在數值真的變動時寫入 Log。第一次呼叫會輸出一整批基準值。
static void trace_track(const tes_snapshot_t *snap, const tes_sm_outputs_t *out,
                        const tes_sm_inputs_t *in)
{
    const uint32_t sid = s_trace_session;
    uint16_t out_v = (uint16_t)(snap->output_voltage * 10.0f + 0.5f);
    uint16_t out_i = (uint16_t)(snap->output_current * 10.0f + 0.5f);
    uint16_t req_i = in->vehicle_status.charge_current_cmd;
    uint16_t req_v = in->vehicle_status.charge_voltage_limit;

    if (!s_trace_prev_valid) {
        s_trace_prev_valid = true;
        trace_svc_logf(sid, "--- baseline state=%s cp=%s soc=%u%%",
                       tr_state(snap->state), tr_cp(snap->cp_state), (unsigned)snap->soc);
        goto store;
    }

    if (snap->state != s_prev.state)
        trace_svc_logf(sid, "STATE %s -> %s", tr_state(s_prev.state), tr_state(snap->state));

    if (snap->cp_state != s_prev.cp_state)
        trace_svc_logf(sid, "CP %s -> %s (%.2fV)",
                       tr_cp(s_prev.cp_state), tr_cp(snap->cp_state), in->cp_voltage);

    if (out->relay_on != s_prev.relay)
        trace_svc_logf(sid, "RELAY %s", out->relay_on ? "CLOSED" : "OPEN");
    if (out->coupler_lock != s_prev.lock)
        trace_svc_logf(sid, "COUPLER LOCK %s", out->coupler_lock ? "ON" : "OFF");
    if (out->vp_relay != s_prev.vp)
        trace_svc_logf(sid, "VP RELAY %s", out->vp_relay ? "ON" : "OFF");

    if (snap->psu_connected != s_prev.psu_connected)
        trace_svc_logf(sid, "PSU %s", snap->psu_connected ? "CONNECTED" : "DISCONNECTED");

    if (snap->can.v500_status != s_prev.v500_status)
        trace_svc_logf(sid, "0x500 status 0x%02X -> 0x%02X",
                       s_prev.v500_status, snap->can.v500_status);
    if (snap->can.v500_fault != s_prev.v500_fault)
        trace_svc_logf(sid, "0x500 FAULT 0x%02X -> 0x%02X",
                       s_prev.v500_fault, snap->can.v500_fault);

    if (snap->can.c508_status != s_prev.c508_status)
        trace_svc_logf(sid, "0x508 status 0x%02X -> 0x%02X",
                       s_prev.c508_status, snap->can.c508_status);
    if (snap->can.c508_fault != s_prev.c508_fault)
        trace_svc_logf(sid, "0x508 FAULT 0x%02X -> 0x%02X",
                       s_prev.c508_fault, snap->can.c508_fault);

    if (req_v != s_prev.v500_req_voltage_01v)
        trace_svc_logf(sid, "BMS Vlimit %.1fV -> %.1fV",
                       s_prev.v500_req_voltage_01v / 10.0f, req_v / 10.0f);

    if ((uint16_t)abs((int)req_i - (int)s_prev.v500_req_current_01a) >= TRACE_DREQ_01A)
        trace_svc_logf(sid, "BMS Ireq %.1fA -> %.1fA",
                       s_prev.v500_req_current_01a / 10.0f, req_i / 10.0f);

    if (snap->soc != s_prev.soc)
        trace_svc_logf(sid, "SOC %u%% -> %u%%", (unsigned)s_prev.soc, (unsigned)snap->soc);

    if ((uint16_t)abs((int)out_v - (int)s_prev.out_v_01v) >= TRACE_DV_01V)
        trace_svc_logf(sid, "Vout %.1fV -> %.1fV", s_prev.out_v_01v / 10.0f, out_v / 10.0f);

    if ((uint16_t)abs((int)out_i - (int)s_prev.out_i_01a) >= TRACE_DI_01A)
        trace_svc_logf(sid, "Iout %.1fA -> %.1fA", s_prev.out_i_01a / 10.0f, out_i / 10.0f);

    if (out->set_psu_voltage && out->psu_voltage_target != s_prev.psu_set_v)
        trace_svc_logf(sid, "PSU SET V=%.1f", out->psu_voltage_target);
    if (out->set_psu_current && out->psu_current_target != s_prev.psu_set_i)
        trace_svc_logf(sid, "PSU SET I=%.1f", out->psu_current_target);

    if (snap->fault_source != s_prev.fault_source && snap->fault_source != 0)
        trace_svc_logf(sid, "FAULT SOURCE=%u flags=0x%02X",
                       (unsigned)snap->fault_source, snap->last_fault_flags);

store:
    s_prev.state                = snap->state;
    s_prev.cp_state             = snap->cp_state;
    s_prev.fault_source         = snap->fault_source;
    s_prev.relay                = out->relay_on;
    s_prev.lock                 = out->coupler_lock;
    s_prev.vp                   = out->vp_relay;
    s_prev.psu_connected        = snap->psu_connected;
    s_prev.v500_status          = snap->can.v500_status;
    s_prev.v500_fault           = snap->can.v500_fault;
    s_prev.c508_status          = snap->can.c508_status;
    s_prev.c508_fault           = snap->can.c508_fault;
    s_prev.v500_req_voltage_01v = req_v;
    s_prev.soc                  = snap->soc;
    // 只在真的記錄過才更新門檻類數值，否則緩慢漂移會永遠達不到門檻
    if ((uint16_t)abs((int)req_i - (int)s_prev.v500_req_current_01a) >= TRACE_DREQ_01A)
        s_prev.v500_req_current_01a = req_i;
    if ((uint16_t)abs((int)out_v - (int)s_prev.out_v_01v) >= TRACE_DV_01V)
        s_prev.out_v_01v = out_v;
    if ((uint16_t)abs((int)out_i - (int)s_prev.out_i_01a) >= TRACE_DI_01A)
        s_prev.out_i_01a = out_i;
    if (out->set_psu_voltage) s_prev.psu_set_v = out->psu_voltage_target;
    if (out->set_psu_current) s_prev.psu_set_i = out->psu_current_target;
}

static void trace_sample_now(const tes_snapshot_t *snap, const tes_sm_inputs_t *in)
{
    trace_sample_t s = {
        .session_id      = s_trace_session,
        .t_ms            = in->tick_ms - s_trace_session_t0,
        .voltage_01v     = (uint16_t)(snap->output_voltage * 10.0f + 0.5f),
        .current_01a     = (uint16_t)(snap->output_current * 10.0f + 0.5f),
        .req_current_01a = in->vehicle_status.charge_current_cmd,
        .soc             = snap->soc,
        .state           = (uint8_t)snap->state,
    };
    trace_svc_add_sample(&s);
    s_trace_last_sample = in->tick_ms;
}

static void drain_can_rx_queue(tes_sm_inputs_t *in, uint32_t now_ms)
{
    can_frame_t frame;
    while (xQueueReceive(g_can_rx_queue, &frame, 0) == pdTRUE) {
        switch (frame.id) {
        case 0x500:
            tes_codec_decode_vehicle_status(frame.data, frame.dlc, &in->vehicle_status);
            s_rx_500++; s_rx_500_ms = now_ms; s_rx_500_seen = true;
            break;
        case 0x501:
            tes_codec_decode_vehicle_params(frame.data, frame.dlc, &in->vehicle_params);
            s_rx_501++; s_rx_501_ms = now_ms; s_rx_501_seen = true;
            break;
        case 0x5F0:
            tes_codec_decode_vehicle_emergency(frame.data, frame.dlc, &s_last_5f0);
            in->vehicle_emergency = (s_last_5f0.error_request_flags & 0x01) != 0;
            s_rx_5f0++; s_rx_5f0_ms = now_ms; s_rx_5f0_seen = true;
            break;
        default: break;
        }
    }
}

// CAN 送出失敗（TX queue 滿、bus-off）以前是完全靜默的。
// 這裡改為節流記錄，避免在無車端連線時每 100ms 洗版。
static void can_send_checked(const can_frame_t *f, uint32_t *ok_counter)
{
    static uint32_t fail_streak = 0;
    if (can_driver_send(f, 0) == ESP_OK) {
        if (ok_counter) (*ok_counter)++;
        if (fail_streak) {
            ESP_LOGI(TAG, "CAN TX recovered after %lu failures",
                     (unsigned long)fail_streak);
            fail_streak = 0;
        }
        return;
    }
    s_tx_fail++;
    if ((++fail_streak % 100u) == 1u) {
        ESP_LOGW(TAG, "CAN TX 0x%03lX failed (%lu consecutive)",
                 (unsigned long)f->id, (unsigned long)fail_streak);
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
        can_send_checked(&f, &s_tx_508);
    }
    if (out->tx_charger_params) {
        can_frame_t f = { .id = 0x509, .dlc = 8 };
        tes_codec_encode_charger_params(&out->params_509, f.data);
        can_send_checked(&f, &s_tx_509);
    }
    if (out->tx_emergency) {
        can_frame_t f = { .id = 0x5F8, .dlc = 8 };
        tes_codec_encode_charger_emergency(&out->emergency_5f8, f.data);
        can_send_checked(&f, &s_tx_5f8);
    }
}

void task_tes_sm(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "started");

    // static：charger_config_t 約 500 bytes，放堆疊上等於每 tick 佔掉
    // task_tes_sm 八分之一的空間。這個任務只有一個實例，static 沒有重入問題。
    static charger_config_t cfg;
    config_svc_get_copy(&cfg);
    tes_sm_config_t sm_cfg = {
        .max_voltage_01v   = cfg.max_voltage_01v,
        .max_current_01a   = cfg.max_current_01a,
        .target_soc        = cfg.target_soc,
        .manufacturer_code = 0x0001,
    };
    tes_sm_init(&s_sm, &sm_cfg);

    tes_sm_inputs_t  inputs;
    tes_sm_outputs_t outputs;
    memset(&inputs, 0, sizeof(inputs));

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));

        inputs.tick_ms = platform_tick_ms();

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

        // 必須在 drain queue 之後才讀取：EVT_FAULT_CLEAR 會清除 g_emergency_stop，
        // 若先讀取則本 tick 仍會看到舊的 true，復歸就得多等一個 tick 才生效。
        inputs.emergency_requested = atomic_load(&g_emergency_stop);

        drain_can_rx_queue(&inputs, inputs.tick_ms);

        inputs.cp_voltage       = g_adc_cp_voltage;
        inputs.cp_sample_seq    = g_adc_cp_seq;
        inputs.measured_voltage = g_adc_output_voltage;

        psu_status_t psu = psu_driver_get_status();
        inputs.psu_connected = psu.connected;
        inputs.psu_voltage   = psu.voltage;
        inputs.psu_current   = psu.current;

        // 一致性快照：這些值會直接進入 0x508/0x509 廣播給 BMS，
        // 不能出現「新電壓 + 舊電流」這種被 setter 寫到一半的組合。
        config_svc_get_copy(&cfg);
        inputs.max_voltage_01v    = cfg.max_voltage_01v;
        inputs.max_current_01a    = cfg.max_current_01a;
        inputs.target_soc         = cfg.target_soc;
        inputs.stop_mode          = cfg.stop_mode;
        inputs.stop_voltage_01v   = cfg.stop_voltage_01v;
        inputs.charge_timer_min   = cfg.charge_timer_min;
        inputs.auto_start_enabled = cfg.auto_start;

        tes_sm_tick(&s_sm, &inputs, &outputs);
        execute_outputs(&outputs);

        // Cache last sent TX frames for CAN diag
        if (outputs.tx_charger_status) s_last_508       = outputs.status_508;
        if (outputs.tx_charger_params)  s_last_509       = outputs.params_509;
        if (outputs.tx_emergency)       s_last_5f8       = outputs.emergency_5f8;

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
        snap.can.v501_soc          = inputs.vehicle_params.soc;
        snap.can.v501_max_time     = inputs.vehicle_params.max_charge_time_min;
        snap.can.v501_eta          = inputs.vehicle_params.est_end_time_min;
        snap.can.v5f0_flags        = s_last_5f0.error_request_flags;
        snap.can.v5f0_max_current  = s_last_5f0.max_charge_current_01a / 10.0f;
        snap.can.v5f0_maker        = s_last_5f0.manufacturer_id;
        // CAN diag — TX side
        snap.can.c508_fault        = s_last_508.fault_flags;
        snap.can.c508_status       = s_last_508.status_flags;
        snap.can.c508_avail_voltage= s_last_508.available_voltage / 10.0f;
        snap.can.c508_avail_current= s_last_508.available_current / 10.0f;
        snap.can.c508_fault_voltage= s_last_508.fault_detect_voltage / 10.0f;
        snap.can.c509_seq          = s_last_509.seq_num;
        snap.can.c509_rated_kw     = s_last_509.rated_power_50w;
        snap.can.c509_voltage      = s_last_509.actual_voltage / 10.0f;
        snap.can.c509_current      = s_last_509.actual_current / 10.0f;
        snap.can.c509_remaining    = s_last_509.remaining_time_min;
        snap.can.c5f8_flags        = s_last_5f8.emergency_flags;
        snap.can.c5f8_maker        = s_last_5f8.manufacturer_id;
        // CAN diag — 收發活性與匯流排健康度
        snap.can.rx_500_count = s_rx_500;
        snap.can.rx_501_count = s_rx_501;
        snap.can.rx_5f0_count = s_rx_5f0;
        snap.can.rx_500_age_ms = age_of(s_rx_500_seen, s_rx_500_ms, inputs.tick_ms);
        snap.can.rx_501_age_ms = age_of(s_rx_501_seen, s_rx_501_ms, inputs.tick_ms);
        snap.can.rx_5f0_age_ms = age_of(s_rx_5f0_seen, s_rx_5f0_ms, inputs.tick_ms);
        snap.can.tx_508_count = s_tx_508;
        snap.can.tx_509_count = s_tx_509;
        snap.can.tx_5f8_count = s_tx_5f8;
        snap.can.tx_fail_count = s_tx_fail;
        {
            can_health_t h;
            can_driver_get_health(&h);
            snap.can.bus_state     = h.state;
            snap.can.bus_tx_err    = h.tx_err;
            snap.can.bus_rx_err    = h.rx_err;
            snap.can.bus_arb_lost  = h.arb_lost;
            snap.can.bus_err_count = h.bus_err;
            snap.can.bus_rx_missed = h.rx_missed;
        }
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

        // Trace：數值變動 Log（每 tick 比對，只有真的變了才寫）
        // + 充電中每 5s 取一次曲線樣本
        if (s_trace_session != 0) {
            trace_track(&snap, &outputs, &inputs);
            if (snap.state == TES_STATE_CHARGING &&
                (uint32_t)(inputs.tick_ms - s_trace_last_sample) >= TRACE_SAMPLE_PERIOD_MS) {
                trace_sample_now(&snap, &inputs);
            }
        }

        static tes_state_t s_last_state = TES_STATE_IDLE;
        if (snap.state != s_last_state) {
            tes_state_t prev  = s_last_state;
            tes_state_t curr  = snap.state;
            // 順序必須與 tes_state_t 完全一致（tes_types.h）：
            // IDLE, PARAM_EXCHANGE, PRE_CHARGE, CHARGING, ENDING, FAULT, EMERGENCY, FINALIZE
            static const char *state_names[] = {
                "IDLE","PARAM_EXCHANGE","PRE_CHARGE","CHARGING",
                "ENDING","FAULT","EMERGENCY","FINALIZE"
            };
            const int n_states = (int)(sizeof(state_names) / sizeof(state_names[0]));
            const char *name      = ((int)curr < n_states) ? state_names[curr] : "?";
            const char *prev_name = ((int)prev < n_states) ? state_names[prev] : "?";
            ESP_LOGI(TAG, "state: %s -> %s  psu_conn=%d cp_v=%.2f fault_src=%u",
                     prev_name, name,
                     inputs.psu_connected, inputs.cp_voltage,
                     (unsigned)snap.fault_source);
            s_last_state = curr;

            // Clear stale CAN data from IDLE auto-start broadcast period on session start
            if (curr == TES_STATE_PARAM_EXCHANGE && prev == TES_STATE_IDLE) {
                memset(&inputs.vehicle_status, 0, sizeof(inputs.vehicle_status));
                memset(&inputs.vehicle_params, 0, sizeof(inputs.vehicle_params));

                // Trace session 從 PARAM_EXCHANGE 起算（不是 CHARGING）——
                // 握手階段正是故障最常發生的地方，Log 必須涵蓋它。
                s_trace_session     = trace_svc_session_begin();
                s_trace_session_t0  = inputs.tick_ms;
                s_trace_last_sample = inputs.tick_ms;
                s_trace_prev_valid  = false;
                memset(&s_prev, 0, sizeof(s_prev));
            }

            // 每次狀態轉換都補一個樣本，讓曲線保有轉折點
            if (s_trace_session != 0) {
                trace_sample_now(&snap, &inputs);
            }

            // Session start
            if (curr == TES_STATE_CHARGING && !s_session_active) {
                s_session_active    = true;
                s_session_energy_wh = 0.0f;
                s_soc_start         = snap.soc;
                s_session_psu_seen  = false;
            }
            // Capture duration + output voltage the tick we leave CHARGING —
            // by the time we reach IDLE the relay is open and both read ~0.
            if (prev == TES_STATE_CHARGING) {
                s_session_end_sec = snap.elapsed_seconds;
                s_session_end_v   = snap.output_voltage;
            }
            // Session end: publish when reaching terminal state
            if (s_session_active && (curr == TES_STATE_IDLE ||
                                     curr == TES_STATE_FAULT ||
                                     curr == TES_STATE_EMERGENCY)) {
                // 停止原因由狀態機在 enter_ending()/enter_fault()/enter_emergency()
                // 直接記錄，這裡不再從 stop_mode 反推（舊做法會把使用者手動停止
                // 誤記成 NORMAL/TIMER/VOLTAGE，且 STOP_REASON_USER/BMS 永遠不會出現）。
                uint8_t reason = snap.stop_reason;
                float   stop_v = (reason == STOP_REASON_VOLTAGE) ? s_session_end_v : 0.0f;

                charge_session_t sess = {
                    .duration_s       = s_session_end_sec,
                    .energy_wh        = s_session_energy_wh,
                    .stop_voltage_v   = stop_v,
                    .session_id       = s_trace_session,   // 連結到 trace 的曲線／Log
                    .soc_start        = s_soc_start,
                    .soc_end          = snap.soc,
                    .stop_reason      = reason,
                    .energy_estimated = s_session_psu_seen ? 0 : 1,
                    // 故障診斷一併存進紀錄，讓歷史頁面能直接說明停止原因
                    .fault_source     = snap.fault_source,
                    .fault_ctx_a      = snap.fault_ctx_a,
                };
                charger_event_t sess_evt = { .type = EVT_SESSION_COMPLETE,
                                             .timestamp_ms = inputs.tick_ms };
                // charge_session_t 目前正好等於 payload 上限。再加欄位的話
                // 這個 memcpy 會靜默寫過頭，因此在編譯期就擋下來。
                _Static_assert(sizeof(charge_session_t) <= sizeof(sess_evt.payload),
                               "charge_session_t exceeds charger_event_t.payload");
                memcpy(sess_evt.payload, &sess, sizeof(charge_session_t));
                event_bus_publish(&sess_evt);
                s_session_active = false;
            }

            charger_event_t evt = { .type = EVT_TES_STATE_CHANGED,
                                    .timestamp_ms = inputs.tick_ms };
            evt.payload[0] = (uint8_t)curr;
            event_bus_publish(&evt);

            // Trace session 結束：回到 IDLE 代表整個流程（含失敗的握手）已收尾。
            // 必須放在 EVT_SESSION_COMPLETE 發佈之後，那裡還要用 s_trace_session。
            if (curr == TES_STATE_IDLE && s_trace_session != 0) {
                trace_svc_session_end(s_trace_session);
                s_trace_session = 0;
            }
        }
    }
}
