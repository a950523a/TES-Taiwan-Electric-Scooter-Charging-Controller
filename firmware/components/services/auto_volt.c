// auto_volt.c — 見 auto_volt.h
#include "services/auto_volt.h"
#include <string.h>

#define SAMPLE_MS        100u
#define HIST_MS          1000u
#define SETTLE_RISE_01V  3u        // 2 秒內上升 < 0.3 V 視為穩定
#define BOOT_TIMEOUT_MS  10000u   // 到了還在爬就先用目前最高值，剩下的交給待機追蹤
#define IDLE_HYST_01V    2u        // 待機追蹤要高出 0.2 V 才更新，免得雜訊讓它一路往上爬

void auto_volt_init(auto_volt_t *a, uint16_t min_01v, uint16_t max_01v, uint32_t now_ms)
{
    memset(a, 0, sizeof(*a));
    a->min_01v        = min_01v;
    a->max_01v        = max_01v;
    a->start_ms       = now_ms;
    a->last_sample_ms = now_ms - SAMPLE_MS;   // 第一次呼叫就取樣
    a->last_hist_ms   = now_ms;
}

// 每 100 ms 收一筆。回傳 true 表示 ring 已滿、*avg 是有效的 1 秒平均
static bool sample(auto_volt_t *a, uint16_t v_01v, uint32_t now_ms, uint16_t *avg)
{
    if (now_ms - a->last_sample_ms >= SAMPLE_MS) {
        a->last_sample_ms = now_ms;
        a->ring[a->ring_i] = v_01v;
        a->ring_i = (uint8_t)((a->ring_i + 1u) % AUTO_VOLT_RING);
        if (a->ring_n < AUTO_VOLT_RING) a->ring_n++;
    }
    if (a->ring_n < AUTO_VOLT_RING) return false;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < AUTO_VOLT_RING; i++) sum += a->ring[i];
    *avg = (uint16_t)((sum + AUTO_VOLT_RING / 2u) / AUTO_VOLT_RING);
    return true;
}

static uint16_t in_range(const auto_volt_t *a, uint16_t v)
{
    return (v >= a->min_01v && v <= a->max_01v) ? v : 0u;
}

bool auto_volt_boot_step(auto_volt_t *a, uint16_t v_01v, uint32_t now_ms, uint16_t *result_01v)
{
    uint16_t avg;
    if (sample(a, v_01v, now_ms, &avg)) {
        if (avg > a->best_01v) a->best_01v = avg;
        if (now_ms - a->last_hist_ms >= HIST_MS) {
            a->last_hist_ms = now_ms;
            if (a->hist_n == AUTO_VOLT_HIST) {
                memmove(&a->hist[0], &a->hist[1], (AUTO_VOLT_HIST - 1u) * sizeof(a->hist[0]));
                a->hist_n--;
            }
            a->hist[a->hist_n++] = avg;
            // hist[0] 是 2 秒前、hist[2] 是現在。電壓下降（負載、雜訊）也算穩定
            if (a->hist_n == AUTO_VOLT_HIST &&
                a->hist[2] < (uint16_t)(a->hist[0] + SETTLE_RISE_01V)) {
                *result_01v = in_range(a, a->best_01v);
                return true;
            }
        }
    }
    if (now_ms - a->start_ms >= BOOT_TIMEOUT_MS) {
        *result_01v = in_range(a, a->best_01v);
        return true;
    }
    return false;
}

uint16_t auto_volt_idle_step(auto_volt_t *a, uint16_t v_01v, uint32_t now_ms,
                             bool idle, uint16_t current_01v)
{
    if (!idle) {
        a->ring_n = 0;            // 只用 IDLE 的讀值：充電中的電壓是電池電壓，不是空載
        a->ring_i = 0;
        return 0;
    }
    uint16_t avg;
    if (!sample(a, v_01v, now_ms, &avg)) return 0;
    avg = in_range(a, avg);
    return (avg >= current_01v + IDLE_HYST_01V) ? avg : 0u;
}
