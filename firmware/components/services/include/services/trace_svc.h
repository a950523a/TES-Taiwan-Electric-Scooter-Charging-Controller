#pragma once
// trace_svc.h — 充電曲線取樣 + 數值變動事件 Log
//
// 兩個 ring buffer 都配置在 PSRAM（8MB OPI）。
//
// ⚠️ 揮發性：重開機後曲線與 Log 會清空。
//    NVS 只有 20KB 且沒有檔案系統分割區，時序資料放不下；要持久化必須改分割表，
//    而改分割表需要整片重燒（bootloader + partition-table + app），會斷掉現有
//    裝置的 OTA 升級路徑。NVS 的 charge_session_t 摘要（log_svc）不受影響。
//
// 寫入端只有 task_tes_sm（單一 writer）；讀取端是 HTTP task。
// 讀取以「一次一筆、短暫持鎖」的方式進行，避免長時間阻塞 10ms 的狀態機 tick。

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

// ── 除錯用總開關 ──────────────────────────────────────────────────────────────
// 改成 0 重新編譯，trace_svc_init() 會直接回報失敗，之後所有記錄函式都變 no-op
// （trace_svc_is_ready() == false），充電流程完全不受影響。
//
// 用途：懷疑當機來自曲線／Log 功能時，這是最快的二分法 —— 關掉後若不再當機，
// 問題就在 trace 這條路徑；若照樣當機，元兇在別處。
#ifndef TES_TRACE_ENABLE
#define TES_TRACE_ENABLE 1
#endif

// ── 曲線取樣 ──────────────────────────────────────────────────────────────────

typedef struct {
    uint32_t session_id;      // 所屬充電 session（0 = 非充電期間）
    uint32_t t_ms;            // 相對 session 起點的毫秒數
    uint16_t voltage_01v;     // 實際輸出電壓
    uint16_t current_01a;     // 實際輸出電流
    uint16_t req_current_01a; // BMS 請求電流（0x500）
    uint8_t  soc;
    uint8_t  state;           // tes_state_t
} trace_sample_t;             // 16 bytes

// ── 變動事件 Log ──────────────────────────────────────────────────────────────

#define TRACE_LOG_TEXT_LEN 76

typedef struct {
    uint32_t session_id;      // 所屬 session（0 = 非充電期間）
    uint32_t t_ms;            // 開機至今毫秒數（單調遞增，一定有值）
    uint32_t epoch_s;         // NTP 已同步時的 UNIX 時間；未同步為 0
    char     text[TRACE_LOG_TEXT_LEN];
} trace_event_t;              // 88 bytes

// ── Session 索引 ──────────────────────────────────────────────────────────────

typedef struct {
    uint32_t session_id;
    uint32_t start_t_ms;      // 開機至今毫秒數
    uint32_t start_epoch_s;   // 0 = 當時尚未 NTP 同步
    uint32_t sample_count;
    uint32_t event_count;
} trace_session_info_t;

#define TRACE_MAX_SESSIONS 8  // 索引中保留的最近 session 數

// ── API ───────────────────────────────────────────────────────────────────────

// 配置 PSRAM buffer。PSRAM 不可用時會退回較小的內部 RAM 配置；
// 兩者皆失敗則 trace 功能停用（所有記錄函式變成 no-op），不影響充電。
esp_err_t trace_svc_init(void);

bool trace_svc_is_ready(void);

// session 生命週期（由 task_tes_sm 呼叫）
uint32_t trace_svc_session_begin(void);   // 回傳新的 session_id（已持久化於 NVS）
void     trace_svc_session_end(uint32_t session_id);

// 記錄（單一 writer：task_tes_sm）
void trace_svc_add_sample(const trace_sample_t *s);
void trace_svc_logf(uint32_t session_id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// 讀取（HTTP task）
uint32_t trace_svc_newest_session(void);
int      trace_svc_get_sessions(trace_session_info_t *out, int max);

// 取第 index 筆（0 = 該 session 最舊的一筆）。回傳 false 表示已到結尾。
bool trace_svc_get_sample(uint32_t session_id, uint32_t index, trace_sample_t *out);
bool trace_svc_get_event (uint32_t session_id, uint32_t index, trace_event_t  *out);

uint32_t trace_svc_sample_count(uint32_t session_id);
uint32_t trace_svc_event_count (uint32_t session_id);

void trace_svc_clear(void);
