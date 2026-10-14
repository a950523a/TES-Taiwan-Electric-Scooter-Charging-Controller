#include "services/live_progress.h"

static int clamp100(long v)
{
    return v < 0 ? 0 : v > 100 ? 100 : (int)v;
}

int live_progress_pct(const live_inputs_t *in)
{
    switch (in->stop_mode) {
    case 1: {   // STOP_MODE_VOLTAGE
        // 停止電壓不高於起點（設定錯或電池已經滿）：沒有可走的距離，視為完成
        if (in->stop_v01 <= in->v0_01) return 100;
        return clamp100(((long)in->v01 - in->v0_01) * 100 / ((long)in->stop_v01 - in->v0_01));
    }
    case 2:     // STOP_MODE_TIMER
        if (in->timer_min == 0) return 0;
        return clamp100((long)in->elapsed_s * 100 / ((long)in->timer_min * 60));
    default:    // STOP_MODE_SOC
        if (in->target_soc <= 0) return 0;
        return clamp100((long)in->soc * 100 / in->target_soc);
    }
}

bool live_should_send(int pct, int last_pct, uint32_t now_ms, uint32_t last_ms, bool first)
{
    if (first) return true;
    return pct != last_pct && (uint32_t)(now_ms - last_ms) >= LIVE_MIN_INTERVAL_MS;
}
