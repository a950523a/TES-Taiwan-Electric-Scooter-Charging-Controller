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
#define CP_READ_MS           50u

// ─── 私有函數原型 ──────────────────────────────────────────────────────────────

static cp_state_t  update_cp_state(tes_sm_t *sm, float cp_v);
static bool        check_battery_compatibility(tes_sm_t *sm, const tes_sm_inputs_t *in);
static void        prepare_periodic_tx(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
static void        run_monitoring(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
static void        enter_fault(tes_sm_t *sm, tes_sm_outputs_t *out);
static void        enter_ending(tes_sm_t *sm, tes_sm_outputs_t *out);
static void        enter_emergency(tes_sm_t *sm, tes_sm_outputs_t *out);
static void        update_timer(tes_sm_t *sm, const tes_sm_inputs_t *in);

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

    // CP 更新（每 50ms 一次）
    if (in->tick_ms - sm->last_cp_read_ms >= CP_READ_MS) {
        sm->last_cp_read_ms = in->tick_ms;
        sm->cp_state = update_cp_state(sm, in->cp_voltage);
    }

    // 緊急停止：最高優先，任何狀態都處理
    // 車端 0x5F0 在 IDLE 充電完成後忽略，防止自動恢復後立刻重入
    {
        bool vehicle_emerg = in->vehicle_emergency &&
                             !(sm->state == TES_STATE_IDLE && sm->charge_complete_latched);
        if (in->emergency_requested || vehicle_emerg) {
            if (sm->state != TES_STATE_EMERGENCY) {
                sm->emergency_hw_triggered = in->emergency_requested;
                enter_emergency(sm, out);
                sm->state_start_ms = in->tick_ms;
            }
        }
    }

    // 主狀態機
    switch (sm->state) {

    case TES_STATE_IDLE:
        out->relay_on     = false;
        out->coupler_lock = false;
        out->vp_relay     = false;
        sm->vehicle_ready          = false;
        sm->timer_running          = false;
        sm->precharge_step         = PRECHARGE_STEP_INIT;
        sm->params_509.remaining_time_min = 0xFFFF;

        if (in->start_requested || sm->remote_start) {
            sm->remote_start            = false;
            sm->fault_latched           = false;
            sm->last_fault_flags        = 0;
            sm->charge_complete_latched = false;  // only clear on new start
            out->vp_relay          = true;
            if (sm->cp_state == CP_STATE_OFF || sm->cp_state == CP_STATE_ON) {
                sm->state          = TES_STATE_PARAM_EXCHANGE;
                sm->state_start_ms = in->tick_ms;
                // PSU 電流重置
                out->set_psu_current      = true;
                out->psu_current_target   = 5.0f;
            }
        }
        break;

    case TES_STATE_PARAM_EXCHANGE:
        // VP 繼電器在整個協商期間保持開啟
        out->vp_relay = true;
        // 等待車端 CAN 許可（0x500 statusFlags bit0 = 1）
        if (!sm->vehicle_ready && (in->vehicle_status.status_flags & 0x01)) {
            sm->vehicle_ready = true;
        }
        if (sm->vehicle_ready) {
            if (check_battery_compatibility(sm, in)) {
                sm->state          = TES_STATE_PRE_CHARGE;
                sm->state_start_ms = in->tick_ms;
                // PSU 電壓設為與車端協商後的上限
                out->set_psu_voltage    = true;
                out->psu_voltage_target = (float)sm->status_508.fault_detect_voltage / 10.0f;
            } else {
                sm->status_508.fault_flags |= 0x04;
                enter_fault(sm, out);
                sm->state_start_ms = in->tick_ms;
            }
        } else if (in->tick_ms - sm->state_start_ms > 15000u) {
            sm->status_508.fault_flags |= 0x01;
            enter_fault(sm, out);
            sm->state_start_ms = in->tick_ms;
        }
        break;

    case TES_STATE_PRE_CHARGE: {
        const tes_vehicle_status_t *vs = &in->vehicle_status;

        // 車端正常停止請求
        if (vs->status_flags & 0x08) {
            enter_ending(sm, out);
            sm->state_start_ms = in->tick_ms;
            break;
        }
        // 等待 CP ON 且 CAN 許可
        if (!(sm->cp_state == CP_STATE_ON && (vs->status_flags & 0x01))) {
            if (in->tick_ms - sm->state_start_ms > 20000u) {
                enter_fault(sm, out);
                sm->state_start_ms = in->tick_ms;
            }
            break;
        }

        // 電磁鎖和 VP 繼電器在整個預充電期間保持
        out->vp_relay     = true;
        out->coupler_lock = (sm->precharge_step >= PRECHARGE_STEP_CONTACTOR_WAIT);

        switch (sm->precharge_step) {
        case PRECHARGE_STEP_INIT:
            // 絕緣測試（stub：永遠通過）
            out->coupler_lock = true;
            sm->status_508.status_flags &= ~0x01u;
            sm->status_508.status_flags |=  0x04u; // 電磁鎖鎖定
            out->tx_charger_status  = true;
            out->status_508         = sm->status_508;
            sm->precharge_step      = PRECHARGE_STEP_CONTACTOR_WAIT;
            sm->state_start_ms      = in->tick_ms;
            break;

        case PRECHARGE_STEP_CONTACTOR_WAIT:
            // 等待車端接觸器閉合（statusFlags bit1 = 0 表示閉合）
            if (!(vs->status_flags & 0x02)) {
                sm->relay_delay_start_ms = in->tick_ms;
                sm->precharge_step       = PRECHARGE_STEP_RELAY_DELAY;
            } else if (in->tick_ms - sm->state_start_ms > 10000u) {
                enter_fault(sm, out);
                sm->state_start_ms = in->tick_ms;
            }
            break;

        case PRECHARGE_STEP_RELAY_DELAY:
            if (in->tick_ms - sm->relay_delay_start_ms >= 250u) {
                out->relay_on           = true;
                sm->status_508.status_flags |= 0x02u; // 充電中
                out->tx_charger_status  = true;
                out->status_508         = sm->status_508;
                sm->precharge_step      = PRECHARGE_STEP_COMPLETE;
            }
            break;

        case PRECHARGE_STEP_COMPLETE:
            sm->state          = TES_STATE_CHARGING;
            sm->state_start_ms = in->tick_ms;
            sm->timer_running  = true;
            sm->elapsed_seconds = 0;
            sm->total_seconds   = 0;
            sm->last_timer_ms   = in->tick_ms;
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

        // 每 tick 更新車輛請求值（供 display 用，與 PSU 連線狀態無關）
        {
            float bms_req = (float)in->vehicle_status.charge_current_cmd / 10.0f;
            if (bms_req > 0.0f)
                sm->last_valid_req_current = bms_req;
            if (in->vehicle_status.charge_voltage_limit > 0)
                sm->last_vehicle_voltage_01v = in->vehicle_status.charge_voltage_limit;
        }

        // PSU 電流控制：取 BMS 請求和用戶上限的最小值
        // 只在 BMS 有送有效電流請求時才更新，避免 bms_req=0 把 PSU 電流清零
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
        // 降流、斷繼電器
        if (in->psu_connected) {
            out->set_psu_current    = true;
            out->psu_current_target = 0.0f;
        }
        out->relay_on = false;

        static uint32_t relay_open_delay_ms = 0;
        if (relay_open_delay_ms == 0) {
            relay_open_delay_ms = in->tick_ms;
        }

        const tes_vehicle_status_t *vs = &in->vehicle_status;
        if (in->tick_ms - relay_open_delay_ms >= 250u) {
            sm->status_508.status_flags |=  0x01u;
            sm->status_508.status_flags &= ~0x02u;
            if ((vs->status_flags & 0x02) && sm->cp_state == CP_STATE_OFF) {
                out->coupler_lock = false;
                sm->status_508.status_flags &= ~0x04u;
                relay_open_delay_ms = 0;
                sm->state           = TES_STATE_FINALIZE;
                sm->state_start_ms  = in->tick_ms;
            } else if (in->tick_ms - sm->state_start_ms > 10000u) {
                out->coupler_lock = false;
                sm->status_508.status_flags &= ~0x04u;
                relay_open_delay_ms = 0;
                sm->state           = TES_STATE_FINALIZE;
                sm->state_start_ms  = in->tick_ms;
            }
        }
        break;
    }

    case TES_STATE_FAULT:
        out->relay_on     = false;
        out->coupler_lock = false;
        if (in->tick_ms - sm->state_start_ms > 10000u) {
            sm->state = TES_STATE_IDLE;
            sm->status_508.fault_flags = 0;
        }
        break;

    case TES_STATE_EMERGENCY:
        out->relay_on     = false;
        out->coupler_lock = false;
        out->vp_relay     = false;
        // 手動復歸（硬體按鈕或遠端）
        if (in->fault_clear_requested || sm->remote_fault_clear) {
            sm->remote_fault_clear            = false;
            sm->fault_latched                 = false;
            sm->state                         = TES_STATE_IDLE;
            sm->status_508.fault_flags        = 0;
            sm->params_509.remaining_time_min = 0xFFFF;
        } else if (!sm->emergency_hw_triggered &&
                   in->tick_ms - sm->state_start_ms > 5000u) {
            // 車端 0x5F0 觸發（正常充電結束後車端常見行為）：5秒後自動回 IDLE
            sm->fault_latched                 = false;
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

    // 週期性 CAN 廣播（協議要求 100ms，僅在 PARAM_EXCHANGE 以後）
    if (sm->state >= TES_STATE_PARAM_EXCHANGE && sm->state < TES_STATE_FAULT) {
        prepare_periodic_tx(sm, in, out);
        // Latch vehicle voltage limit for snapshot (0x500 field)
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
    snap.last_valid_req_current = sm->last_valid_req_current;

    if (sm->fault_latched)                               snap.led_state = LED_STATE_FAULT;
    else if (sm->charge_complete_latched)                snap.led_state = LED_STATE_COMPLETE;
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
        return sm->cp_state; // 保持舊狀態直到達到閾值
    }
    sm->cp_error_count = 0;
    return detected;
}

static bool check_battery_compatibility(tes_sm_t *sm, const tes_sm_inputs_t *in)
{
    const tes_vehicle_status_t *vs  = &in->vehicle_status;
    const tes_vehicle_params_t *vp  = &in->vehicle_params;
    uint16_t bms_max = vp->max_charge_voltage;  // from 0x501

    if (in->auto_voltage && bms_max > 0) {
        // 自動模式：以 BMS 回報的最大充電電壓為目標，max_voltage_01v 作為安全上限
        uint16_t target = (bms_max < in->max_voltage_01v) ? bms_max : in->max_voltage_01v;
        sm->status_508.available_voltage    = target;
        sm->status_508.fault_detect_voltage = target;
        return true;
    }

    // 手動模式：以用戶設定的 max_voltage_01v 為上限
    if (vs->charge_voltage_limit > in->max_voltage_01v) return false;
    if (bms_max > 0 && vs->charge_voltage_limit > bms_max) return false;

    uint16_t limit = (bms_max > 0 && bms_max < in->max_voltage_01v)
                     ? bms_max : in->max_voltage_01v;
    sm->status_508.fault_detect_voltage = limit;
    return true;
}

static void prepare_periodic_tx(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out)
{
    if (in->tick_ms - sm->last_periodic_ms < PERIODIC_SEND_MS) return;
    sm->last_periodic_ms = in->tick_ms;

    // 更新 0x509 即時數值
    // 優先使用 PSU 回報值；若 PSU 只送 CMD_ACK（psu_voltage==0），fallback 到 ADC
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

    // CAN 許可撤銷（bit0 消失）
    if (!(vs->status_flags & 0x01)) {
        enter_ending(sm, out); return;
    }
    // 車端正常停止請求（bit3）
    if (vs->status_flags & 0x08) {
        enter_ending(sm, out); return;
    }
    // 車端停止請求（bit2）
    if (vs->status_flags & 0x04) {
        enter_ending(sm, out); return;
    }
    if (in->stop_requested || sm->remote_stop) {
        sm->remote_stop = false;
        enter_ending(sm, out); return;
    }
    if (sm->soc >= (uint8_t)sm->cfg.target_soc) {
        enter_ending(sm, out); return;
    }
    if (in->tick_ms - sm->state_start_ms > VOLTAGE_CHECK_DELAY_MS) {
        float v_limit = (float)vs->charge_voltage_limit / 10.0f;
        float v_out   = in->psu_connected ? in->psu_voltage : in->measured_voltage;
        if (v_limit > 0.1f && v_out >= v_limit + VOLTAGE_CHECK_TOLERANCE) {
            enter_ending(sm, out); return;
        }
    }
    if (sm->timer_running && sm->total_seconds > 0 &&
        sm->elapsed_seconds >= sm->total_seconds) {
        enter_ending(sm, out); return;
    }
    if (vs->fault_flags != 0) {
        sm->last_fault_flags = vs->fault_flags;
        enter_fault(sm, out); return;
    }
    if (sm->cp_state != CP_STATE_ON) {
        enter_fault(sm, out); return;
    }
}

static void enter_fault(tes_sm_t *sm, tes_sm_outputs_t *out)
{
    sm->fault_latched  = true;
    sm->timer_running  = false;
    sm->state          = TES_STATE_FAULT;
    out->relay_on      = false;
    out->coupler_lock  = false;
    if (sm->status_508.fault_flags == 0) sm->status_508.fault_flags = 0x01;
    // 保存故障碼供顯示（若呼叫端已設定則保留，否則使用 CAN 故障旗標）
    if (sm->last_fault_flags == 0) sm->last_fault_flags = sm->status_508.fault_flags;
    out->set_psu_current    = true;
    out->psu_current_target = 5.0f; // PSU 電流重置
}

static void enter_ending(tes_sm_t *sm, tes_sm_outputs_t *out)
{
    sm->charge_complete_latched = true;
    sm->timer_running           = false;
    sm->state                   = TES_STATE_ENDING;
    out->set_psu_current        = true;
    out->psu_current_target     = 0.0f;
}

static void enter_emergency(tes_sm_t *sm, tes_sm_outputs_t *out)
{
    sm->fault_latched  = true;
    sm->timer_running  = false;
    sm->state          = TES_STATE_EMERGENCY;
    out->relay_on      = false;
    out->coupler_lock  = false;
    out->vp_relay      = false;
    out->set_psu_current    = true;
    out->psu_current_target = 0.0f;

    sm->status_508.fault_flags   |= 0x01;
    sm->last_fault_flags          = sm->status_508.fault_flags; // 保存緊急故障碼
    sm->status_508.status_flags  |= 0x01;
    out->tx_charger_status  = true;
    out->tx_emergency       = true;
    out->status_508         = sm->status_508;
    out->emergency_5f8.emergency_flags = 0x01;
    out->emergency_5f8.manufacturer_id = sm->cfg.manufacturer_code;
}

static void update_timer(tes_sm_t *sm, const tes_sm_inputs_t *in)
{
    // 更新 BMS 提供的總充電時間
    if (sm->timer_running) {
        uint16_t bms_time = in->vehicle_params.max_charge_time_min;
        if (bms_time != 0xFFFF) {
            uint32_t new_total = (uint32_t)bms_time * 60u;
            if (sm->total_seconds != new_total) sm->total_seconds = new_total;
        }
        // 秒計時
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
