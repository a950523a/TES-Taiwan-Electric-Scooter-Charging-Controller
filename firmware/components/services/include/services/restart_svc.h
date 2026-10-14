#pragma once
// restart_svc — 使用者要求的重新啟動（OLED 設定選單、網頁 POST /reboot）
//
// 充電流程中（PARAM_EXCHANGE … ENDING）不准重啟：esp_restart() 會讓繼電器與
// 電磁鎖失去控制，PSU 也會停在最後的 setpoint 繼續輸出 —— 和 OTA 前檢查的理由相同。
// 讀不到狀態（快照 mutex 逾時）時一律當作忙碌：寧可拒絕一次，也不在充電中重啟。
#include <stdbool.h>
#include <stdint.h>

// 目前可以重啟嗎（不在充電流程中）
bool restart_svc_allowed(void);

// 檢查 restart_svc_allowed()，可以的話 delay_ms 後重啟並回傳 true。
// 延遲是為了讓 HTTP 回應送得出去、OLED 來得及顯示 Restarting。
bool restart_svc_request(uint32_t delay_ms, const char *who);

// 已排定重啟（OLED 用來顯示 Restarting... 畫面）
bool restart_svc_pending(void);
