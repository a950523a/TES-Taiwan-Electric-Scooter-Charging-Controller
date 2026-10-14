#pragma once
#include <stdbool.h>
#include <stdint.h>

// 手機 App 即時通知（Android Now Bar／Live Updates）的充電進度。純函式，可在主機上測。
//
// 進度依停止方式算（使用者 2026-10-08 指定）：
//   STOP_MODE_SOC     → SOC ÷ 目標 SOC
//   STOP_MODE_VOLTAGE → 從開始充電時的電壓走到停止電壓
//   STOP_MODE_TIMER   → 已充時間 ÷ 設定時間
// 進度每變 1% 送一次，但兩次之間至少隔 LIVE_MIN_INTERVAL_MS。

#define LIVE_MIN_INTERVAL_MS 30000u

typedef struct {
    uint8_t  stop_mode;     // stop_mode_t
    uint8_t  soc;           // %
    int8_t   target_soc;    // %
    uint16_t v01;           // 目前電壓，0.1 V
    uint16_t v0_01;         // 開始充電時的電壓，0.1 V
    uint16_t stop_v01;      // 停止電壓，0.1 V
    uint32_t elapsed_s;
    uint16_t timer_min;
} live_inputs_t;

// 0–100
int  live_progress_pct(const live_inputs_t *in);

// 要不要送：第一次一定送；之後進度變了而且距上次夠久才送
bool live_should_send(int pct, int last_pct, uint32_t now_ms, uint32_t last_ms, bool first);
