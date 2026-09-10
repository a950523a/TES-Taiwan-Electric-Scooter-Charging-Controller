#include "tes_protocol/tes_sm.h"
#include <string.h>

// CP 電壓判斷門檻（與 V2 config.h 一致）
#define CP_OFF_MAX_V        1.9f
#define CP_ON_MIN_V         7.4f
#define CP_ON_MAX_V         13.7f
#define CP_HYSTERESIS       0.3f
#define CP_ERROR_THRESHOLD  2

// 充電中電壓檢查參數
#define VOLTAGE_CHECK_DELAY_MS   1000u
#define VOLTAGE_CHECK_TOLERANCE  0.1f

// CAN 週期發送間隔（協議要求 100ms ± 10ms）
#define PERIODIC_SEND_MS    100u

// ─── 私有函數原型 ──────────────────────────────────────────────────────────────

static cp_state_t  update_cp_state(tes_sm_t *sm, float cp_v);
static bool        check_battery_compatibility(tes_sm_t *sm, const tes_sm_inputs_t *in);
static void        prepare_periodic_tx(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
static void        run_monitoring(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
static void        update_timer(tes_sm_t *sm, const tes_sm_inputs_t *in);

// enter_*() 一律自行設定 state_start_ms —— 呼叫端不得再另外設定。
// （舊版由呼叫端負責，run_monitoring 的路徑漏設，導致 FAULT/ENDING 沿用
//   進入 CHARGING 的時間戳而立刻逾時。）
//
// ctx_a / ctx_b 是故障當下的情境數值，意義依 src 而定（見 fault_source_t）。
// 顯示層靠它把「Code:0x01」變成「車端要求 72.0V，超過設定上限 60.0V」。
static void enter_fault    (tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms,
                            uint8_t src, uint16_t ctx_a, uint16_t ctx_b);
static void enter_ending   (tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms, uint8_t reason);
static void enter_emergency(tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms,
                            uint8_t src, uint16_t ctx_a);

// ─── 公開 API ─────────────────────────────────────────────────────────────────

void tes_sm_init(tes_sm_t *sm, const tes_sm_config_t *cfg)
{
    memset(sm, 0, sizeof(*sm));
    sm->cfg   = *cfg;
    sm->state = TES_STATE_IDLE;

    // 初始化 0x508 廣播值
    sm->status_508.available_voltage     = cfg->max_voltage_01v;
    sm->status_508.available_current     = cfg->max_current_01a;
    sm->status_508.fault_detect_voltage  = cfg->max_voltage_01v;

    // 初始化 0x509 廣播值
    sm->params_509.seq_num            = 18; // 硬編碼
    uint32_t rated_w = (cfg->max_voltage_01v / 10u) * (cfg->max_current_01a / 10u);
    sm->params_509.rated_power_50w    = (uint8_t)(rated_w / 50u);
    sm->params_509.remaining_time_min = 0xFFFF;
}

void tes_sm_tick(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out)
{
    memset(out, 0, sizeof(*out));

    // 每 tick 同步設定值（外部可能已更新）
    sm->cfg.max_voltage_01v = in->max_voltage_01v;
    sm->cfg.max_current_01a = in->max_current_01a;
    sm->cfg.target_soc      = in->target_soc;
    sm->status_508.available_voltage    = in->max_voltage_01v;
    sm->status_508.available_current    = in->max_current_01a;
    sm->status_508.fault_detect_voltage = in->max_voltage_01v;

    // 更新 BMS SOC
    sm->soc = in->vehicle_params.soc;

    // 更新計時器
    update_timer(sm, in);

    // CP 更新：只在 ADC 真的取得新取樣時才跑一次
    // （記錄前一狀態供 auto_start 邊緣偵測及充電完成清除判斷）
    if (in->cp_sample_seq != sm->last_cp_seq) {
        sm->last_cp_seq = in->cp_sample_seq;
        sm->cp_prev     = sm->cp_state;
        sm->cp_state    = update_cp_state(sm, in->cp_voltage);
    }

    // 緊急停止：最高優先，任何狀態都處理
    // 車端 0x5F0 在 IDLE 充電完成後忽略，防止自動恢復後立刻重入
    {
        bool vehicle_emerg = in->vehicle_emergency &&
                             !(sm->state == TES_STATE_IDLE && sm->charge_complete_latched);
        if (in->emergency_requested || vehicle_emerg) {
            if (sm->state != TES_STATE_EMERGENCY) {
                sm->emergency_hw_triggered = in->emergency_requested;
                enter_emergency(sm, out, in->tick_ms,
                                in->emergency_requested ? FAULT_SRC_EMERGENCY_BTN
                                                        : FAULT_SRC_EMERGENCY_VEHICLE,
                                in->emergency_requested ? 0u : 1u);
            }
        }
    }

    // 主狀態機
    switch (sm->state) {

    case TES_STATE_IDLE:
        out->relay_on     = false;
        out->coupler_lock = false;
        // Beta auto_start: VP 常通，讓車端上電後能送 CAN（電流 > 15A 時禁用）
        out->vp_relay     = in->auto_start_enabled && (in->max_current_01a <= 150u);
        sm->vehicle_ready          = false;
        sm->timer_running          = false;
        sm->precharge_step         = PRECHARGE_STEP_INIT;
        sm->params_509.remaining_time_min = 0xFFFF;
        // 確保 IDLE 時 status_508 保持乾淨的待機狀態
        sm->status_508.status_flags = C508_ST_STOP_CONTROL;  // 停止狀態（非充電中、鎖已解除）
        sm->status_508.fault_flags  = 0;

        // Beta: 充電完成後 CP 穩定斷開（兩次讀取皆 OFF）才允許下次自動觸發
        // 同步重置 last_can_permit，確保下次 CAN bit0=1 能偵測到 0→1 邊緣
        if (in->auto_start_enabled && sm->charge_complete_latched &&
            sm->cp_state == CP_STATE_OFF && sm->cp_prev == CP_STATE_OFF) {
            sm->charge_complete_latched = false;
            sm->last_can_permit         = false;
        }

        // 手動 START（按鈕或遠端）：清除故障鎖存，強制進入流程
        if (in->start_requested || sm->remote_start) {
            sm->remote_start            = false;
            sm->fault_latched           = false;
            sm->last_fault_flags        = 0;
            sm->fault_source            = FAULT_SRC_NONE;
            sm->charge_complete_latched = false;
            out->vp_relay = true;
            if (sm->cp_state == CP_STATE_OFF || sm->cp_state == CP_STATE_ON) {
                sm->state          = TES_STATE_PARAM_EXCHANGE;
                sm->state_start_ms = in->tick_ms;
                out->set_psu_current    = true;
                out->psu_current_target = 5.0f;
            }
            break;
        }

        // Beta auto_start：CP OFF→ON 邊緣（CP 比 CAN 更早出現）或 CAN bit0 0→1 邊緣皆可觸發
        // 時序：VP ON → CP ON → CAN bit0=1 → 充電 → CAN ends → CP OFF
        // 不清除 fault_latched — 故障後仍須手動按 START 確認
        if (in->auto_start_enabled && (in->max_current_01a <= 150u) &&
            !sm->fault_latched && !sm->charge_complete_latched) {
            bool cp_edge  = (sm->cp_state == CP_STATE_ON && sm->cp_prev != CP_STATE_ON);
            bool can_edge = (in->vehicle_status.status_flags & V500_ST_CHARGE_PERMIT) && !sm->last_can_permit;
            if (cp_edge || can_edge) {
                out->vp_relay = true;
                sm->state          = TES_STATE_PARAM_EXCHANGE;
                sm->state_start_ms = in->tick_ms;
                out->set_psu_current    = true;
                out->psu_current_target = 5.0f;
            }
        }
        break;

    case TES_STATE_PARAM_EXCHANGE:
        out->vp_relay = true;
        if (!sm->vehicle_ready && (in->vehicle_status.status_flags & V500_ST_CHARGE_PERMIT)) {
            sm->vehicle_ready = true;
        }
        if (sm->vehicle_ready) {
            if (check_battery_compatibility(sm, in)) {
                sm->state          = TES_STATE_PRE_CHARGE;
                sm->state_start_ms = in->tick_ms;
                out->set_psu_voltage    = true;
                out->psu_voltage_target = (float)sm->status_508.fault_detect_voltage / 10.0f;
            } else {
                // 0x508 故障位元由 enter_fault() 依 fault_source 統一決定
                // 留下「車端要多少 / 我們只給到多少」，使用者才知道要把上限調高
                enter_fault(sm, out, in->tick_ms, FAULT_SRC_VOLTAGE_INCOMPAT,
                            in->vehicle_status.charge_voltage_limit, in->max_voltage_01v);
            }
        } else if (in->tick_ms - sm->state_start_ms > 15000u) {
            enter_fault(sm, out, in->tick_ms, FAULT_SRC_VEHICLE_TIMEOUT,
                        (uint16_t)((in->tick_ms - sm->state_start_ms) / 1000u),
                        in->vehicle_status.status_flags);
        }
        break;

    case TES_STATE_PRE_CHARGE: {
        const tes_vehicle_status_t *vs = &in->vehicle_status;

        if (vs->status_flags & V500_ST_NORMAL_STOP_REQ) {
            enter_ending(sm, out, in->tick_ms, STOP_REASON_NORMAL);
            break;
        }
        if (!(sm->cp_state == CP_STATE_ON && (vs->status_flags & V500_ST_CHARGE_PERMIT))) {
            if (in->tick_ms - sm->state_start_ms > 20000u) {
                // CP 電壓 + 車端 status：可分辨是槍沒插好還是車還沒給許可
                enter_fault(sm, out, in->tick_ms, FAULT_SRC_PRECHARGE_TIMEOUT,
                            (uint16_t)(in->cp_voltage * 10.0f), vs->status_flags);
            }
            break;
        }

        out->vp_relay     = true;
        out->coupler_lock = (sm->precharge_step >= PRECHARGE_STEP_CONTACTOR_WAIT);

        switch (sm->precharge_step) {
        case PRECHARGE_STEP_INIT:
            out->coupler_lock = true;
            sm->status_508.status_flags &= ~C508_ST_STOP_CONTROL;
            sm->status_508.status_flags |=  C508_ST_COUPLER_LOCKED;
            out->tx_charger_status  = true;
            out->status_508         = sm->status_508;
            sm->precharge_step      = PRECHARGE_STEP_CONTACTOR_WAIT;
            sm->state_start_ms      = in->tick_ms;
            break;

        case PRECHARGE_STEP_CONTACTOR_WAIT:
            if (!(vs->status_flags & V500_ST_CONTACTOR_OPEN)) {
                sm->relay_delay_start_ms = in->tick_ms;
                sm->precharge_step       = PRECHARGE_STEP_RELAY_DELAY;
            } else if (in->tick_ms - sm->state_start_ms > 10000u) {
                enter_fault(sm, out, in->tick_ms, FAULT_SRC_CONTACTOR_TIMEOUT,
                            (uint16_t)((in->tick_ms - sm->state_start_ms) / 1000u),
                            vs->status_flags);
            }
            break;

        case PRECHARGE_STEP_RELAY_DELAY:
            if (in->tick_ms - sm->relay_delay_start_ms >= 250u) {
                out->relay_on           = true;
                sm->status_508.status_flags |= C508_ST_CHARGING;
                out->tx_charger_status  = true;
                out->status_508         = sm->status_508;
                sm->precharge_step      = PRECHARGE_STEP_COMPLETE;
            }
            break;

        case PRECHARGE_STEP_COMPLETE:
            sm->state                 = TES_STATE_CHARGING;
            sm->state_start_ms        = in->tick_ms;
            sm->timer_running         = true;
            sm->elapsed_seconds       = 0;
            sm->total_seconds         = 0;
            sm->last_timer_ms         = in->tick_ms;
            sm->psu_session_connected = in->psu_connected; // 快照：決定本次充電是否依賴 PSU
            break;

        default:
            break;
        }
        break;
    }

    case TES_STATE_CHARGING:
        out->relay_on     = true;
        out->coupler_lock = true;
        out->vp_relay     = true;

        {
            float bms_req = (float)in->vehicle_status.charge_current_cmd / 10.0f;
            if (bms_req > 0.0f)
                sm->last_valid_req_current = bms_req;
            if (in->vehicle_status.charge_voltage_limit > 0)
                sm->last_vehicle_voltage_01v = in->vehicle_status.charge_voltage_limit;
        }

        if (in->psu_connected) {
            float bms_req = (float)in->vehicle_status.charge_current_cmd / 10.0f;
            if (bms_req > 0.0f) {
                float user_limit = (float)in->max_current_01a / 10.0f;
                float target     = (bms_req < user_limit) ? bms_req : user_limit;
                sm->last_valid_req_current  = bms_req;
                out->set_psu_current    = true;
                out->psu_current_target = target;
            }
        }

        run_monitoring(sm, in, out);
        break;

    case TES_STATE_ENDING: {
        if (in->psu_connected) {
            out->set_psu_current    = true;
            out->psu_current_target = 0.0f;
        }
        out->relay_on = false;

        if (sm->relay_open_delay_ms == 0) {
            sm->relay_open_delay_ms = in->tick_ms;
        }

        const tes_vehicle_status_t *vs = &in->vehicle_status;
        if (in->tick_ms - sm->relay_open_delay_ms >= 250u) {
            sm->status_508.status_flags |=  C508_ST_STOP_CONTROL;
            sm->status_508.status_flags &= ~C508_ST_CHARGING;
            if ((vs->status_flags & V500_ST_CONTACTOR_OPEN) && sm->cp_state == CP_STATE_OFF) {
                out->coupler_lock = false;
                sm->status_508.status_flags &= ~C508_ST_COUPLER_LOCKED;
                sm->relay_open_delay_ms = 0;
                sm->state               = TES_STATE_FINALIZE;
                sm->state_start_ms      = in->tick_ms;
            } else if (in->tick_ms - sm->state_start_ms > 10000u) {
                out->coupler_lock = false;
                sm->status_508.status_flags &= ~C508_ST_COUPLER_LOCKED;
                sm->relay_open_delay_ms = 0;
                sm->state               = TES_STATE_FINALIZE;
                sm->state_start_ms      = in->tick_ms;
            }
        }
        break;
    }

    case TES_STATE_FAULT:
        out->relay_on     = false;
        out->coupler_lock = false;
        out->vp_relay     = in->auto_start_enabled && (in->max_current_01a <= 150u);
        if (in->tick_ms - sm->state_start_ms > sm->fault_timeout_ms) {
            sm->fault_latched          = false;
            sm->state                  = TES_STATE_IDLE;
            sm->status_508.fault_flags = 0;
        }
        break;

    case TES_STATE_EMERGENCY:
        out->relay_on     = false;
        out->coupler_lock = false;
        out->vp_relay     = false;
        if (in->fault_clear_requested || sm->remote_fault_clear) {
            sm->remote_fault_clear            = false;
            sm->fault_latched                 = false;
            sm->fault_source                  = FAULT_SRC_NONE;
            sm->state                         = TES_STATE_IDLE;
            sm->status_508.fault_flags        = 0;
            sm->params_509.remaining_time_min = 0xFFFF;
        } else if (!sm->emergency_hw_triggered &&
                   in->tick_ms - sm->state_start_ms > 5000u) {
            sm->fault_latched                 = false;
            sm->fault_source                  = FAULT_SRC_NONE;
            sm->state                         = TES_STATE_IDLE;
            sm->status_508.fault_flags        = 0;
            sm->params_509.remaining_time_min = 0xFFFF;
        }
        break;

    case TES_STATE_FINALIZE:
        out->relay_on = false;
        sm->state     = TES_STATE_IDLE;
        break;

    default:
        sm->state = TES_STATE_IDLE;
        break;
    }

    // 週期性 CAN 廣播（協議要求 100ms）
    // auto_start IDLE 也廣播 0x508：VP 常通時車端插槍即可立刻收到充電樁回應，
    // 避免車端在 CP ON 後等待 0x508 而超時（等同手動按下 START 後 VP ON 即開始廣播）
    bool should_broadcast = (sm->state >= TES_STATE_PARAM_EXCHANGE && sm->state < TES_STATE_FAULT)
                         || (sm->state == TES_STATE_IDLE && in->auto_start_enabled && in->max_current_01a <= 150u);
    if (should_broadcast) {
        prepare_periodic_tx(sm, in, out);
        if (in->vehicle_status.charge_voltage_limit > 0)
            sm->last_vehicle_voltage_01v = in->vehicle_status.charge_voltage_limit;
    }

    // Live voltage/current: prefer PSU measured; fallback to ADC when PSU not reporting V/I
    if (in->psu_connected && in->psu_voltage > 0.0f) {
        sm->live_voltage_01v = (uint16_t)(in->psu_voltage   * 10.0f);
        sm->live_current_01a = (uint16_t)(in->psu_current   * 10.0f);
    } else {
        sm->live_voltage_01v = (uint16_t)(in->measured_voltage * 10.0f);
        sm->live_current_01a = out->relay_on ? sm->cfg.max_current_01a : 0;
    }

    // Beta auto_start: update CAN permit edge-detection latch for next tick
    sm->last_can_permit = (in->vehicle_status.status_flags & V500_ST_CHARGE_PERMIT) != 0;

    // Remote flags 一律只存活一個 tick。
    // 若某狀態沒有消耗它（例如在 IDLE 收到 remote_stop），舊版會永久殘留，
    // 導致下一次進入 CHARGING 時立刻被停止。
    sm->remote_start       = false;
    sm->remote_stop        = false;
    sm->remote_fault_clear = false;
}

tes_snapshot_t tes_sm_get_snapshot(const tes_sm_t *sm)
{
    tes_snapshot_t snap = {0};
    snap.state                  = sm->state;
    snap.fault_latched          = sm->fault_latched;
    snap.charge_complete        = sm->charge_complete_latched;
    snap.soc                    = sm->soc;
    snap.output_voltage         = sm->live_voltage_01v / 10.0f;
    snap.output_current         = sm->live_current_01a / 10.0f;
    snap.vehicle_req_voltage    = sm->last_vehicle_voltage_01v / 10.0f;
    snap.vehicle_req_current    = sm->last_valid_req_current;
    snap.timer_running          = sm->timer_running;
    snap.elapsed_seconds        = sm->elapsed_seconds;
    snap.total_seconds          = sm->total_seconds;
    snap.remaining_seconds      = (sm->total_seconds > sm->elapsed_seconds)
                                  ? (sm->total_seconds - sm->elapsed_seconds) : 0;
    snap.max_voltage_01v        = sm->cfg.max_voltage_01v;
    snap.max_current_01a        = sm->cfg.max_current_01a;
    snap.target_soc             = sm->cfg.target_soc;
    snap.last_fault_flags       = sm->last_fault_flags;
    snap.fault_source           = sm->fault_source;
    snap.fault_ctx_a            = sm->fault_ctx_a;
    snap.fault_ctx_b            = sm->fault_ctx_b;
    snap.stop_reason            = sm->last_stop_reason;
    snap.cp_state               = (uint8_t)sm->cp_state;
    snap.last_valid_req_current = sm->last_valid_req_current;

    // charge_complete_latched 同時扮演 auto_start 的重入防護，使用者手動停止時
    // 也會被設起來 —— 但那不算「充電完成」，LED 不應該亮綠燈。
    bool completed_ok = sm->charge_complete_latched &&
                        (sm->last_stop_reason == STOP_REASON_NORMAL  ||
                         sm->last_stop_reason == STOP_REASON_TIMER   ||
                         sm->last_stop_reason == STOP_REASON_VOLTAGE);

    if (sm->fault_latched)                               snap.led_state = LED_STATE_FAULT;
    else if (completed_ok)                               snap.led_state = LED_STATE_COMPLETE;
    else if (sm->state == TES_STATE_CHARGING)            snap.led_state = LED_STATE_CHARGING;
    else                                                  snap.led_state = LED_STATE_STANDBY;

    return snap;
}

void tes_sm_request_start       (tes_sm_t *sm) { sm->remote_start       = true; }
void tes_sm_request_stop        (tes_sm_t *sm) { sm->remote_stop        = true; }
void tes_sm_request_fault_clear (tes_sm_t *sm) { sm->remote_fault_clear = true; }

// ─── 私有函數實作 ─────────────────────────────────────────────────────────────

static cp_state_t update_cp_state(tes_sm_t *sm, float cp_v)
{
    cp_state_t detected;
    if      (cp_v >= 0.0f && cp_v <= (CP_OFF_MAX_V - CP_HYSTERESIS)) detected = CP_STATE_OFF;
    else if (cp_v >= (CP_ON_MIN_V + CP_HYSTERESIS) && cp_v <= CP_ON_MAX_V) detected = CP_STATE_ON;
    else    detected = CP_STATE_ERROR;

    if (detected == CP_STATE_ERROR) {
        if (++sm->cp_error_count >= CP_ERROR_THRESHOLD) return CP_STATE_ERROR;
        return sm->cp_state;
    }
    sm->cp_error_count = 0;
    return detected;
}

static bool check_battery_compatibility(tes_sm_t *sm, const tes_sm_inputs_t *in)
{
    const tes_vehicle_status_t *vs = &in->vehicle_status;
    if (vs->charge_voltage_limit > in->max_voltage_01v) return false;
    if (vs->max_charge_voltage > 0 &&
        vs->charge_voltage_limit > vs->max_charge_voltage) return false;

    // If max_charge_voltage=0 (not yet received), fall back to our own max to avoid VLIM2=0
    // which would cause the vehicle to flag fault_flags=0x01 whenever output voltage > 0
    uint16_t limit = (vs->max_charge_voltage > 0 && vs->max_charge_voltage < in->max_voltage_01v)
                     ? vs->max_charge_voltage : in->max_voltage_01v;
    sm->status_508.fault_detect_voltage = limit;
    return true;
}

static void prepare_periodic_tx(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out)
{
    if (in->tick_ms - sm->last_periodic_ms < PERIODIC_SEND_MS) return;
    sm->last_periodic_ms = in->tick_ms;

    if (in->psu_connected && in->psu_voltage > 0.0f) {
        sm->params_509.actual_voltage = (uint16_t)(in->psu_voltage * 10.0f);
        sm->params_509.actual_current = (uint16_t)(in->psu_current * 10.0f);
    } else {
        sm->params_509.actual_voltage = (uint16_t)(in->measured_voltage * 10.0f);
        sm->params_509.actual_current = out->relay_on
            ? sm->cfg.max_current_01a : 0;
    }

    uint32_t rem = (sm->total_seconds > sm->elapsed_seconds)
                   ? (sm->total_seconds - sm->elapsed_seconds) : 0;
    sm->params_509.remaining_time_min = sm->timer_running
                                        ? (uint16_t)((rem + 30u) / 60u)
                                        : 0xFFFF;

    out->tx_charger_status = true;
    out->tx_charger_params = true;
    out->status_508        = sm->status_508;
    out->params_509        = sm->params_509;
}

static void run_monitoring(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out)
{
    const tes_vehicle_status_t *vs = &in->vehicle_status;

    // PSU 斷線：只有充電開始時已連線的情況才 FAULT（中途斷線）；
    // 開始時本就無 PSU 則維持 ADC-only 模式，不中斷充電
    if (sm->psu_session_connected && !in->psu_connected) {
        enter_fault(sm, out, in->tick_ms, FAULT_SRC_PSU_LOST, (uint16_t)sm->elapsed_seconds, 0);
        return;
    }

    if (!(vs->status_flags & V500_ST_CHARGE_PERMIT)) {
        enter_ending(sm, out, in->tick_ms, STOP_REASON_BMS); return;
    }
    if (vs->status_flags & V500_ST_NORMAL_STOP_REQ) {
        enter_ending(sm, out, in->tick_ms, STOP_REASON_NORMAL); return;
    }
    if (vs->status_flags & V500_ST_POSTURE_NG) {
        enter_ending(sm, out, in->tick_ms, STOP_REASON_BMS); return;
    }
    if (in->stop_requested || sm->remote_stop) {
        sm->remote_stop = false;
        enter_ending(sm, out, in->tick_ms, STOP_REASON_USER); return;
    }
    if (in->stop_mode == STOP_MODE_VOLTAGE) {
        float v_out = (in->psu_connected && in->psu_voltage > 0.0f)
                      ? in->psu_voltage : in->measured_voltage;
        if (v_out >= (float)in->stop_voltage_01v / 10.0f) {
            enter_ending(sm, out, in->tick_ms, STOP_REASON_VOLTAGE); return;
        }
    } else if (in->stop_mode == STOP_MODE_TIMER) {
        if (sm->elapsed_seconds >= (uint32_t)in->charge_timer_min * 60u) {
            enter_ending(sm, out, in->tick_ms, STOP_REASON_TIMER); return;
        }
    } else {
        if (sm->soc >= (uint8_t)sm->cfg.target_soc) {
            enter_ending(sm, out, in->tick_ms, STOP_REASON_NORMAL); return;
        }
    }
    if (in->tick_ms - sm->state_start_ms > VOLTAGE_CHECK_DELAY_MS) {
        float v_limit = (float)vs->charge_voltage_limit / 10.0f;
        float v_out   = in->psu_connected ? in->psu_voltage : in->measured_voltage;
        if (v_limit > 0.1f && v_out >= v_limit + VOLTAGE_CHECK_TOLERANCE) {
            enter_ending(sm, out, in->tick_ms, STOP_REASON_VOLTAGE); return;
        }
    }
    if (sm->timer_running && sm->total_seconds > 0 &&
        sm->elapsed_seconds >= sm->total_seconds) {
        enter_ending(sm, out, in->tick_ms, STOP_REASON_TIMER); return;
    }
    // 忽略充電開始後 2 秒內的車端故障旗標：
    // 車端 fault_flags=0x01 可能在充電初期因 IDLE 廣播期間殘留的協議狀態而短暫出現，
    // 待車端確認充電樁 status_flags=0x06（充電中+電磁鎖）後才穩定清除
    if (vs->fault_flags != 0 &&
        in->tick_ms - sm->state_start_ms > 2u * VOLTAGE_CHECK_DELAY_MS) {
        sm->last_fault_flags = vs->fault_flags;
        enter_fault(sm, out, in->tick_ms, FAULT_SRC_BMS_FAULT,
                    vs->fault_flags, vs->status_flags);
        return;
    }
    if (sm->cp_state != CP_STATE_ON) {
        if (in->auto_start_enabled) {
            // auto_start 模式：CP 斷開視為車端主動結束的正常停止
            enter_ending(sm, out, in->tick_ms, STOP_REASON_NORMAL);
        } else {
            enter_fault(sm, out, in->tick_ms, FAULT_SRC_CP_LOST,
                        (uint16_t)(in->cp_voltage * 10.0f), (uint16_t)sm->cp_state);
            sm->fault_timeout_ms = 1000u; // 手動模式：CP 斷開快速復歸（1 秒）
        }
        return;
    }
}

// 依故障來源選出要送給車端的 0x508 故障位元。
// 協定只給三個位元（供電系統 / 裝置本體 / 電池不適合），對不上的一律歸到
// bit0 供電系統異常 —— 它語意最廣、最不會誤導；絕不把車端的問題謊報成
// bit1「直流供電裝置異常」（那等於自認充電樁壞掉）。
static uint8_t c508_fault_bit(uint8_t src)
{
    switch ((fault_source_t)src) {
    case FAULT_SRC_VOLTAGE_INCOMPAT:
        // 車端要的電壓超出我們能給的範圍 → 對車端而言就是「電池不適合」
        return C508_FAULT_BATTERY_UNSUIT;
    case FAULT_SRC_PSU_LOST:
    case FAULT_SRC_EMERGENCY_BTN:
        // PSU 就是「直流供電裝置」；緊急停止也是本機主動切斷
        return C508_FAULT_DEVICE_ABNORMAL;
    default:
        return C508_FAULT_SUPPLY_SYSTEM;
    }
}

static void enter_fault(tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms,
                        uint8_t src, uint16_t ctx_a, uint16_t ctx_b)
{
    sm->fault_latched     = true;
    sm->timer_running     = false;
    sm->fault_timeout_ms  = 10000u;
    sm->state             = TES_STATE_FAULT;
    sm->state_start_ms    = tick_ms;   // 必須在此設定：FAULT 逾時以進入 FAULT 的時刻為基準
    sm->fault_source      = src;
    sm->fault_ctx_a       = ctx_a;
    sm->fault_ctx_b       = ctx_b;
    sm->last_stop_reason  = STOP_REASON_FAULT;
    out->relay_on         = false;
    out->coupler_lock     = false;
    sm->status_508.fault_flags |= c508_fault_bit(src);
    if (sm->last_fault_flags == 0) sm->last_fault_flags = sm->status_508.fault_flags;
    out->set_psu_current    = true;
    out->psu_current_target = 0.0f;
}

static void enter_ending(tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms, uint8_t reason)
{
    sm->charge_complete_latched = true;
    sm->timer_running           = false;
    sm->relay_open_delay_ms     = 0;
    sm->state                   = TES_STATE_ENDING;
    sm->state_start_ms          = tick_ms;  // ENDING 的 10s 逃生逾時以此為基準
    sm->last_stop_reason        = reason;
    out->set_psu_current        = true;
    out->psu_current_target     = 0.0f;
}

static void enter_emergency(tes_sm_t *sm, tes_sm_outputs_t *out, uint32_t tick_ms,
                            uint8_t src, uint16_t ctx_a)
{
    sm->fault_latched  = true;
    sm->timer_running  = false;
    sm->state          = TES_STATE_EMERGENCY;
    sm->state_start_ms = tick_ms;   // 車端 0x5F0 的 5s 自動復歸以此為基準
    sm->fault_source   = src;
    sm->fault_ctx_a    = ctx_a;
    sm->fault_ctx_b    = 0;
    sm->last_stop_reason = STOP_REASON_EMERG;
    out->relay_on      = false;
    out->coupler_lock  = false;
    out->vp_relay      = false;
    out->set_psu_current    = true;
    out->psu_current_target = 0.0f;

    sm->status_508.fault_flags   |= c508_fault_bit(src);
    sm->last_fault_flags          = sm->status_508.fault_flags;
    sm->status_508.status_flags  |= C508_ST_STOP_CONTROL;
    out->tx_charger_status  = true;
    out->tx_emergency       = true;
    out->status_508         = sm->status_508;
    out->emergency_5f8.emergency_flags = 0x01;
    out->emergency_5f8.manufacturer_id = sm->cfg.manufacturer_code;
}

static void update_timer(tes_sm_t *sm, const tes_sm_inputs_t *in)
{
    if (sm->timer_running) {
        uint16_t bms_time = in->vehicle_params.max_charge_time_min;
        if (bms_time != 0xFFFF) {
            uint32_t new_total = (uint32_t)bms_time * 60u;
            if (sm->total_seconds != new_total) sm->total_seconds = new_total;
        }
        uint32_t delta_ms = in->tick_ms - sm->last_timer_ms;
        if (delta_ms >= 1000u) {
            sm->elapsed_seconds += delta_ms / 1000u;
            sm->last_timer_ms   += (delta_ms / 1000u) * 1000u;
        }
    } else {
        sm->elapsed_seconds = 0;
        sm->last_timer_ms   = in->tick_ms;
    }
}
