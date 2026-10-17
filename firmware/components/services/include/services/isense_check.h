#pragma once
// isense_check — 電流讀不到的偵測（2026-10-17）
//
// 純邏輯、不碰硬體：呼叫端每個節拍餵狀態與讀值。電流量測節點的分流電阻多半只量單一方向，
// 負極線接反時充電照常、什麼都不會壞，但電流永遠讀 0 —— 0x509 回報 0 A、過電流保護也看不到。
//
// 判斷：CHARGING 中車端請求 ≥ 2 A，實測卻連續 10 秒 < 0.3 A。請求電流和開始時的爬升都
// 在 10 秒內，正常接線不會觸發。電源沒有輸出（旋鈕電壓比電池低）看起來一模一樣，所以提示的是
// 「檢查接線或電源」，不只是接反。觸發後一直保持到斷電重開 —— 改接線本來就要斷電。
// 觸發後呼叫端把實測電流當作不可信（current_measured = false），退回沒有實測時的做法。
#include <stdbool.h>
#include <stdint.h>

#define ISENSE_CHECK_REQ_01A   20u      // 車端請求 ≥ 2.0 A 才判斷
#define ISENSE_CHECK_LOW_MA    300      // 實測 < 0.3 A 算「讀不到」
#define ISENSE_CHECK_MS        10000u   // 連續 10 秒

typedef struct {
    uint32_t low_since_ms;
    bool     low;          // 目前這一段是否正在累計
    bool     tripped;      // 已觸發（保持到重開機）
} isense_check_t;

void isense_check_init(isense_check_t *c);

// 每個節拍呼叫。charging = 狀態機在 CHARGING；req_01a = 車端 0x500 請求電流；
// meas_ma = 實測電流。剛觸發的那一拍回傳 true（給呼叫端印 log）。
bool isense_check_step(isense_check_t *c, bool charging, uint16_t req_01a, int32_t meas_ma,
                       uint32_t now_ms);
