#include "services/wifi_plan.h"
#include <string.h>

void wifi_plan_init(wifi_plan_t *p, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->down_since_ms = now_ms;
    p->down_known    = true;
}

void wifi_plan_retry_soon(wifi_plan_t *p, uint32_t now_ms)
{
    p->retry_at_ms = now_ms + WIFI_PLAN_RETRY_MS;
    p->retry_pending = true;
}

void wifi_plan_tick(wifi_plan_t *p, const wifi_plan_in_t *in, wifi_plan_out_t *out)
{
    uint32_t now = in->now_ms;
    out->scan = false;
    out->ap_secure = true;

    if (!in->sta_wanted) {
        // 首次設定或使用者固定熱點：開放、不掃描（跟以前一樣）
        out->ap_on = true;
        out->ap_secure = false;
        p->down_known = false;
        p->ap_idle_known = false;
        return;
    }

    if (in->sta_up) {
        p->ever_up = true;
        p->down_known = false;
        // 連上了：熱點沒人用 30 秒就關
        if (!in->ap_on) {
            out->ap_on = false;
            p->ap_idle_known = false;
        } else if (in->ap_clients > 0) {
            out->ap_on = true;
            p->ap_idle_known = false;
        } else {
            if (!p->ap_idle_known) { p->ap_idle_since_ms = now; p->ap_idle_known = true; }
            out->ap_on = (uint32_t)(now - p->ap_idle_since_ms) < WIFI_PLAN_AP_IDLE_OFF_MS;
        }
        return;
    }

    p->ap_idle_known = false;
    if (!p->down_known) { p->down_since_ms = now; p->down_known = true; }
    uint32_t delay = p->ever_up ? WIFI_PLAN_AP_DELAY_LOST_MS : WIFI_PLAN_AP_DELAY_BOOT_MS;
    out->ap_on = in->ap_on || (uint32_t)(now - p->down_since_ms) >= delay;

    if (in->hold || in->sta_busy) return;
    uint32_t interval = out->ap_on ? WIFI_PLAN_SCAN_SLOW_MS : WIFI_PLAN_SCAN_FAST_MS;
    bool due = p->retry_pending ? (int32_t)(now - p->retry_at_ms) >= 0
                                : (!p->scanned || (uint32_t)(now - p->last_scan_ms) >= interval);
    if (due) {
        out->scan = true;
        p->scanned = true;
        p->retry_pending = false;
        p->last_scan_ms = now;
    }
}
