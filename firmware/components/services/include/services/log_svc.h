#pragma once
#include "tes_protocol/tes_types.h"
#include "services/energy_month.h"
#include <esp_err.h>
#include <stdint.h>

// 一筆充電紀錄：TES-Protocol 的 charge_session_t 再加上開始時間。開始時間只有這個韌體用得到，
// 所以包在這裡，不去改三個 repo 共用的子模組
typedef struct {
    charge_session_t s;
    uint32_t         start_epoch;   // 開始充電（進入 CHARGING）的 Unix 秒；0 = 一直沒對到時
    float            ah;            // 充入 Ah（2026-10-22）；< 0 = 不知道（之前的紀錄）
} charge_record_t;                  // 32 bytes

// EVT_SESSION_COMPLETE 的 payload。start_uptime_ms 只在 RAM 用：開始時還沒對時的話，
// 這次開機之內一對到時，log_svc 就用它回推開始時間（與 trace 時間回推同一個做法）
typedef struct {
    charge_record_t rec;
    uint32_t        start_uptime_ms;   // esp_timer 毫秒（= platform_tick_ms()）
} session_event_t;                     // 36 bytes = charger_event_t.payload

esp_err_t log_svc_init       (void);
uint8_t   log_svc_get_history(charge_record_t *buf, uint8_t max);   // newest-first
void      log_svc_get_energy (energy_book_t *out);                  // 月累計，由舊到新

// 電費換算（網頁、App 的月圖表用）：電源效率 %（50–100，預設 90）與每度電價（0.01 元，0 = 不換算）
uint8_t   log_svc_psu_eff_pct  (void);
uint16_t  log_svc_kwh_price_c  (void);
void      log_svc_set_cost     (uint8_t eff_pct, uint16_t price_c);

void      task_log           (void *arg);
