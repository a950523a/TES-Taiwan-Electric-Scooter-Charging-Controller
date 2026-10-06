#pragma once
// auto_volt — 自動偵測電源的空載電壓（Auto Volt），結果拿來當 Max Voltage
//
// 純邏輯、不碰硬體：呼叫端每個節拍餵電壓讀值與時間。
//
// 為什麼不是「開機讀一次」：控制板由電源本身供電（120 V → 12 V 降壓模組），
// 開機時電源常常還在爬升，有些電源要好幾秒才到穩態，讀一次就會抓到偏低的值。
//
//   * 每 100 ms 取樣，用最近 1 秒的平均 —— 濾掉突波。結果會寫進 0x508 的 VLIM2
//     （車端的過壓保護門檻），單次讀值的最大值可能是突波，太高會削弱車端保護
//   * 開機：每秒記一次平均，「現在」比「2 秒前」上升不到 0.3 V 就算穩定，取過程中
//     最高的 1 秒平均；最多等 10 秒。電源一開始就穩定的話約 3 秒。爬得很慢的電源
//     （時間常數 8 秒以上）10 秒內量到的會偏低，由下面的待機追蹤補上 —— 偏低是安全的方向
//   * 待機（IDLE，車端接觸器斷開、沒有負載）：持續取樣，1 秒平均比目前值高就往上
//     更新。只往上、不往下；離開 IDLE 就清掉取樣，充電中的讀值不會混進來
#include <stdbool.h>
#include <stdint.h>

#define AUTO_VOLT_RING   10u   // 10 × 100 ms = 1 秒平均
#define AUTO_VOLT_HIST   3u    // 每秒一筆，比較「現在」與「2 秒前」

typedef struct {
    uint16_t min_01v, max_01v;          // 有效範圍；範圍外的結果不採用
    uint16_t ring[AUTO_VOLT_RING];
    uint8_t  ring_n, ring_i;
    uint16_t hist[AUTO_VOLT_HIST];
    uint8_t  hist_n;
    uint32_t start_ms, last_sample_ms, last_hist_ms;
    uint16_t best_01v;                  // 開機期間最高的 1 秒平均
} auto_volt_t;

void auto_volt_init(auto_volt_t *a, uint16_t min_01v, uint16_t max_01v, uint32_t now_ms);

// 開機：反覆呼叫直到回傳 true（穩定或逾時）。*result_01v = 偵測到的電壓，
// 不在有效範圍內時為 0（呼叫端保留原本的設定）。
bool auto_volt_boot_step(auto_volt_t *a, uint16_t v_01v, uint32_t now_ms, uint16_t *result_01v);

// 待機追蹤：每個節拍呼叫。idle = 狀態機在 IDLE。回傳比 current_01v 高、
// 在有效範圍內的新值；沒有要更新時回傳 0。
uint16_t auto_volt_idle_step(auto_volt_t *a, uint16_t v_01v, uint32_t now_ms,
                             bool idle, uint16_t current_01v);
