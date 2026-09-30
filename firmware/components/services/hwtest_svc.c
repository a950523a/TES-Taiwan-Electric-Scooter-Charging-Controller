// hwtest_svc.c — 工作台測試模式的要求與租約。規則見 hwtest_svc.h。
//
// 兩個任務會碰這份狀態：network（HTTP 要求）與 tes_sm（每 10ms 判斷並套用）。
// 狀態只有十幾個位元組、臨界區只做指派，用 portMUX 自旋鎖即可 —— 這裡不碰
// PSRAM，也不會拖到 SM 的 tick。

#include "services/hwtest_svc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>

static const char *TAG = "hwtest";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool           active;
    uint8_t        mask;
    uint32_t       owner;
    uint32_t       lease_until_ms;
    hwtest_block_t block;      // task_tes_sm 最近一次算出的阻擋條件
    hwtest_block_t last_end;
    hwtest_block_t pending_end; // HTTP 端結束的原因，留給 tick 記 log（HTTP 端不記）
    bool           has_ended;
    uint32_t       ended_at_ms;
} s = {
    // SM 還沒跑第一個 tick 之前不知道能不能接管，先當作不行
    .block = HWTEST_BLOCK_NOT_IDLE,
};

// 租約比較要容忍 uint32 回繞（開機 49.7 天後）
static bool expired(uint32_t now_ms)
{
    return (int32_t)(now_ms - s.lease_until_ms) >= 0;
}

hwtest_block_t hwtest_svc_start(uint32_t owner, uint32_t now_ms)
{
    hwtest_block_t r;
    taskENTER_CRITICAL(&s_lock);
    r = s.block;
    if (r == HWTEST_OK) {
        if (s.active && s.owner != owner) s.pending_end = HWTEST_END_REPLACED;
        s.active         = true;
        s.owner          = owner;
        s.mask           = 0;   // 一律從「全部關」開始，由頁面逐項打開
        s.lease_until_ms = now_ms + HWTEST_LEASE_MS;
    }
    taskEXIT_CRITICAL(&s_lock);
    return r;
}

bool hwtest_svc_set(uint32_t owner, uint8_t mask, uint32_t now_ms)
{
    bool ok;
    taskENTER_CRITICAL(&s_lock);
    ok = s.active && s.owner == owner;
    if (ok) {
        s.mask           = mask & HWTEST_OUT_ALL;
        s.lease_until_ms = now_ms + HWTEST_LEASE_MS;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

bool hwtest_svc_keepalive(uint32_t owner, uint32_t now_ms)
{
    bool ok;
    taskENTER_CRITICAL(&s_lock);
    ok = s.active && s.owner == owner;
    if (ok) s.lease_until_ms = now_ms + HWTEST_LEASE_MS;
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

void hwtest_svc_stop(uint32_t owner)
{
    taskENTER_CRITICAL(&s_lock);
    if (s.active && s.owner == owner) {
        s.active      = false;
        s.mask        = 0;
        s.last_end    = HWTEST_END_USER;
        s.pending_end = HWTEST_END_USER;
    }
    taskEXIT_CRITICAL(&s_lock);
}

hwtest_status_t hwtest_svc_status(uint32_t now_ms)
{
    hwtest_status_t st;
    taskENTER_CRITICAL(&s_lock);
    st.active        = s.active;
    st.mask          = s.active ? s.mask : 0;
    st.lease_left_ms = (s.active && !expired(now_ms)) ? s.lease_until_ms - now_ms : 0;
    st.block         = s.block;
    st.last_end      = s.last_end;
    st.owner         = s.active ? s.owner : 0;
    st.ended_ago_ms  = s.has_ended ? now_ms - s.ended_at_ms : UINT32_MAX;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

bool hwtest_svc_tick(hwtest_block_t block, uint32_t now_ms, uint8_t *mask)
{
    bool           active;
    uint8_t        m;
    hwtest_block_t ended = HWTEST_OK;

    taskENTER_CRITICAL(&s_lock);
    s.block = block;
    if (s.active) {
        if (block != HWTEST_OK)  ended = block;
        else if (expired(now_ms)) ended = HWTEST_END_LEASE;
        if (ended != HWTEST_OK) {
            s.active   = false;
            s.mask     = 0;
            s.last_end = ended;
        }
    }
    if (ended == HWTEST_OK && s.pending_end != HWTEST_OK) ended = s.pending_end;
    s.pending_end = HWTEST_OK;
    // 「被取代」不算結束 —— 測試還在進行，只是換了頁面
    if (ended != HWTEST_OK && ended != HWTEST_END_REPLACED) {
        s.has_ended   = true;
        s.ended_at_ms = now_ms;
    }
    active = s.active;
    m      = s.mask;
    taskEXIT_CRITICAL(&s_lock);

    // log 放在臨界區外：ESP_LOGx 會用掉 1KB 以上的堆疊，也不能在關中斷時跑
    if (ended != HWTEST_OK)
        ESP_LOGW(TAG, "bench test ended: %s — outputs back to the state machine",
                 hwtest_block_name(ended));

    *mask = active ? m : 0;
    return active;
}

const char *hwtest_block_name(hwtest_block_t b)
{
    switch (b) {
    case HWTEST_OK:                 return "ok";
    case HWTEST_BLOCK_NOT_IDLE:     return "not_idle";
    case HWTEST_BLOCK_EMERGENCY:    return "emergency";
    case HWTEST_BLOCK_CP_PRESENT:   return "cp_present";
    case HWTEST_BLOCK_VEHICLE_CAN:  return "vehicle_can";
    case HWTEST_END_LEASE:          return "lease";
    case HWTEST_END_USER:           return "user";
    case HWTEST_END_REPLACED:       return "replaced";
    }
    return "?";
}
