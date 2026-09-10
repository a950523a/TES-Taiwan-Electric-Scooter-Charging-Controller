// trace_svc.c — 充電曲線取樣 + 數值變動事件 Log（PSRAM ring buffer）
//
// 設計重點：
//  * 單一 writer（task_tes_sm）、多 reader（HTTP task）。
//  * 用 mutex 而非 portMUX：臨界區內要存取 PSRAM，而 taskENTER_CRITICAL 會關中斷，
//    在 cache 可能被停用的情境下存取 PSRAM 有風險；mutex 沒有這個問題。
//  * writer 取鎖有逾時，逾時就丟棄該筆記錄 —— 記錄永遠不可以拖慢 10ms 的狀態機。
//  * 每筆記錄都帶 session_id，並以「絕對序號」定位，讀取為 O(1)，
//    且能偵測出該格是否已被後續寫入覆蓋。

#include "services/trace_svc.h"
#include "hal/hal_nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static const char *TAG = "trace_svc";

#define NVS_NS         "tes_hist"
#define NVS_KEY_SEQ    "sess_seq"

// PSRAM 配置量：8192×16B = 128KB  +  6144×88B = 528KB  ≈ 656KB
#define SAMPLE_CAP_PSRAM   8192u
#define EVENT_CAP_PSRAM    6144u
// 無 PSRAM 時的退回值（內部 RAM，僅供基本診斷）
#define SAMPLE_CAP_FALLBACK 256u
#define EVENT_CAP_FALLBACK  128u

#define LOCK_TIMEOUT_WRITER pdMS_TO_TICKS(5)
#define LOCK_TIMEOUT_READER pdMS_TO_TICKS(50)

// ── 內部 session 索引 ─────────────────────────────────────────────────────────

typedef struct {
    uint32_t session_id;
    uint32_t start_t_ms;
    uint32_t start_epoch_s;
    uint32_t sample_first_seq;
    uint32_t sample_count;
    uint32_t event_first_seq;
    uint32_t event_count;
    bool     in_use;
} sess_slot_t;

// ── 狀態 ──────────────────────────────────────────────────────────────────────

static trace_sample_t *s_samples   = NULL;
static trace_event_t  *s_events    = NULL;
static uint32_t        s_sample_cap = 0;
static uint32_t        s_event_cap  = 0;
static uint32_t        s_sample_seq = 0;   // 已寫入的樣本總數（絕對序號）
static uint32_t        s_event_seq  = 0;
static SemaphoreHandle_t s_lock     = NULL;

static sess_slot_t s_sessions[TRACE_MAX_SESSIONS];
static int         s_sess_head = 0;        // 下一個要覆寫的槽位
static uint32_t    s_next_session_id  = 1;
static uint32_t    s_session_id_limit = 0; // 本次開機預留到的最大 id

// 開機時一次向 NVS 預留一整段 session id，之後都從 RAM 發號。
//
// 原本是每次 session 開始就寫一次 NVS —— 但那是在 task_tes_sm 裡，
// nvs_commit() 會做 flash 寫入，既吃掉 1~2KB 堆疊（4KB 的任務直接爆掉），
// 又會阻塞數十毫秒，讓 10ms 的安全迴圈停擺。
#define SESSION_ID_RESERVE 256

// ── 小工具 ────────────────────────────────────────────────────────────────────

// 與 platform_tick_ms() 同一個時基（esp_timer），確保 event 的 t_ms
// 和 task_tes_sm 傳進來的 sample t_ms 可以直接相減。
static inline uint32_t now_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static uint32_t now_epoch_s(void)
{
    time_t now = time(NULL);
    // 2020-09-13 之後才視為已由 NTP 校時；否則回 0 代表「只有開機時間可用」
    return (now > 1600000000) ? (uint32_t)now : 0u;
}

static sess_slot_t *find_slot(uint32_t session_id)
{
    if (session_id == 0) return NULL;
    for (int i = 0; i < TRACE_MAX_SESSIONS; i++) {
        if (s_sessions[i].in_use && s_sessions[i].session_id == session_id)
            return &s_sessions[i];
    }
    return NULL;
}

// 判斷絕對序號 seq 是否還沒被環形覆蓋
static inline bool seq_alive(uint32_t seq, uint32_t write_seq, uint32_t cap)
{
    return (uint32_t)(write_seq - seq) <= cap;
}

// ── 初始化 ────────────────────────────────────────────────────────────────────

esp_err_t trace_svc_init(void)
{
#if !TES_TRACE_ENABLE
    // 二分法用：整個 trace 停用，所有記錄函式因 is_ready()==false 變成 no-op
    ESP_LOGW(TAG, "TES_TRACE_ENABLE=0 — charge curve / log disabled at compile time");
    return ESP_ERR_NOT_SUPPORTED;
#else
    memset(s_sessions, 0, sizeof(s_sessions));

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed — trace disabled");
        return ESP_ERR_NO_MEM;
    }

    s_samples = heap_caps_malloc(SAMPLE_CAP_PSRAM * sizeof(trace_sample_t), MALLOC_CAP_SPIRAM);
    s_events  = heap_caps_malloc(EVENT_CAP_PSRAM  * sizeof(trace_event_t),  MALLOC_CAP_SPIRAM);
    if (s_samples && s_events) {
        s_sample_cap = SAMPLE_CAP_PSRAM;
        s_event_cap  = EVENT_CAP_PSRAM;
        ESP_LOGI(TAG, "PSRAM buffers: %u samples (%uKB) + %u events (%uKB)",
                 (unsigned)s_sample_cap,
                 (unsigned)(s_sample_cap * sizeof(trace_sample_t) / 1024),
                 (unsigned)s_event_cap,
                 (unsigned)(s_event_cap * sizeof(trace_event_t) / 1024));
    } else {
        // PSRAM 不可用 → 退回內部 RAM 的小 buffer，功能降級但不影響充電
        free(s_samples); free(s_events);
        s_samples = calloc(SAMPLE_CAP_FALLBACK, sizeof(trace_sample_t));
        s_events  = calloc(EVENT_CAP_FALLBACK,  sizeof(trace_event_t));
        if (!s_samples || !s_events) {
            free(s_samples); free(s_events);
            s_samples = NULL; s_events = NULL;
            ESP_LOGE(TAG, "buffer alloc failed — trace disabled");
            return ESP_ERR_NO_MEM;
        }
        s_sample_cap = SAMPLE_CAP_FALLBACK;
        s_event_cap  = EVENT_CAP_FALLBACK;
        ESP_LOGW(TAG, "PSRAM unavailable — degraded to %u samples / %u events in internal RAM",
                 (unsigned)s_sample_cap, (unsigned)s_event_cap);
    }

    // 續用 NVS 中的 session 序號，讓重開機後的 id 不會和舊的充電紀錄撞號。
    // 這裡（app_main 內，堆疊充裕）一次預留一整段，之後 session_begin 純 RAM 運作。
    uint32_t seq = 0;
    hal_nvs_get_u32(NVS_NS, NVS_KEY_SEQ, &seq);
    s_next_session_id  = seq + 1;
    s_session_id_limit = seq + SESSION_ID_RESERVE;
    hal_nvs_set_u32(NVS_NS, NVS_KEY_SEQ, s_session_id_limit);

    ESP_LOGI(TAG, "session ids %u..%u reserved",
             (unsigned)s_next_session_id, (unsigned)s_session_id_limit);
    return ESP_OK;
#endif
}

bool trace_svc_is_ready(void)
{
    return s_samples != NULL && s_events != NULL && s_lock != NULL;
}

// ── Session 生命週期 ──────────────────────────────────────────────────────────

uint32_t trace_svc_session_begin(void)
{
    if (!trace_svc_is_ready()) return 0;

    // 純 RAM 發號 —— 這個函式是從 task_tes_sm 的 10ms tick 呼叫的，
    // 絕對不能在這裡碰 NVS（見 SESSION_ID_RESERVE 的說明）。
    uint32_t id = s_next_session_id++;
    if (id > s_session_id_limit) {
        // 單次開機用掉 256 個 session 才會發生；id 在本次開機內仍唯一，
        // 只是重開機後理論上可能與舊紀錄撞號。trace buffer 只留 8 個 session，
        // 實務上到不了這裡。
        ESP_LOGW(TAG, "session id reservation exhausted (%u)", (unsigned)id);
    }

    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_WRITER) != pdTRUE) return 0;
    sess_slot_t *sl = &s_sessions[s_sess_head];
    s_sess_head = (s_sess_head + 1) % TRACE_MAX_SESSIONS;
    memset(sl, 0, sizeof(*sl));
    sl->session_id       = id;
    sl->start_t_ms       = now_uptime_ms();
    sl->start_epoch_s    = now_epoch_s();
    sl->sample_first_seq = s_sample_seq;
    sl->event_first_seq  = s_event_seq;
    sl->in_use           = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "session %u begin", (unsigned)id);
    return id;
}

void trace_svc_session_end(uint32_t session_id)
{
    if (!trace_svc_is_ready() || session_id == 0) return;
    ESP_LOGI(TAG, "session %u end (%u samples, %u events)",
             (unsigned)session_id,
             (unsigned)trace_svc_sample_count(session_id),
             (unsigned)trace_svc_event_count(session_id));
}

// ── 寫入 ──────────────────────────────────────────────────────────────────────

void trace_svc_add_sample(const trace_sample_t *s)
{
    if (!trace_svc_is_ready() || !s) return;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_WRITER) != pdTRUE) return;  // 寧可漏記，不可拖慢 SM

    s_samples[s_sample_seq % s_sample_cap] = *s;
    s_sample_seq++;

    sess_slot_t *sl = find_slot(s->session_id);
    if (sl) sl->sample_count++;

    xSemaphoreGive(s_lock);
}

void trace_svc_logf(uint32_t session_id, const char *fmt, ...)
{
    if (!trace_svc_is_ready() || !fmt) return;

    // 先在堆疊上格式化，避免把 vsnprintf 的耗時放進臨界區
    char buf[TRACE_LOG_TEXT_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint32_t t_ms    = now_uptime_ms();
    uint32_t epoch_s = now_epoch_s();

    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_WRITER) != pdTRUE) return;

    trace_event_t *e = &s_events[s_event_seq % s_event_cap];
    e->session_id = session_id;
    e->t_ms       = t_ms;
    e->epoch_s    = epoch_s;
    memcpy(e->text, buf, TRACE_LOG_TEXT_LEN);
    e->text[TRACE_LOG_TEXT_LEN - 1] = '\0';
    s_event_seq++;

    sess_slot_t *sl = find_slot(session_id);
    if (sl) sl->event_count++;

    xSemaphoreGive(s_lock);
}

// ── 讀取 ──────────────────────────────────────────────────────────────────────

uint32_t trace_svc_newest_session(void)
{
    if (!trace_svc_is_ready()) return 0;
    uint32_t best = 0;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return 0;
    for (int i = 0; i < TRACE_MAX_SESSIONS; i++) {
        if (s_sessions[i].in_use && s_sessions[i].session_id > best)
            best = s_sessions[i].session_id;
    }
    xSemaphoreGive(s_lock);
    return best;
}

int trace_svc_get_sessions(trace_session_info_t *out, int max)
{
    if (!trace_svc_is_ready() || !out || max <= 0) return 0;
    int n = 0;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return 0;
    for (int i = 0; i < TRACE_MAX_SESSIONS && n < max; i++) {
        const sess_slot_t *sl = &s_sessions[i];
        if (!sl->in_use) continue;
        out[n].session_id    = sl->session_id;
        out[n].start_t_ms    = sl->start_t_ms;
        out[n].start_epoch_s = sl->start_epoch_s;
        out[n].sample_count  = sl->sample_count;
        out[n].event_count   = sl->event_count;
        n++;
    }
    xSemaphoreGive(s_lock);

    // 由新到舊排序（槽位是環形覆寫，順序不保證）
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].session_id > out[i].session_id) {
                trace_session_info_t t = out[i]; out[i] = out[j]; out[j] = t;
            }
    return n;
}

uint32_t trace_svc_sample_count(uint32_t session_id)
{
    if (!trace_svc_is_ready()) return 0;
    uint32_t c = 0;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return 0;
    const sess_slot_t *sl = find_slot(session_id);
    if (sl) c = sl->sample_count;
    xSemaphoreGive(s_lock);
    return c;
}

uint32_t trace_svc_event_count(uint32_t session_id)
{
    if (!trace_svc_is_ready()) return 0;
    uint32_t c = 0;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return 0;
    const sess_slot_t *sl = find_slot(session_id);
    if (sl) c = sl->event_count;
    xSemaphoreGive(s_lock);
    return c;
}

bool trace_svc_get_sample(uint32_t session_id, uint32_t index, trace_sample_t *out)
{
    if (!trace_svc_is_ready() || !out) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return false;

    const sess_slot_t *sl = find_slot(session_id);
    if (sl && index < sl->sample_count) {
        uint32_t seq = sl->sample_first_seq + index;
        if (seq_alive(seq, s_sample_seq, s_sample_cap)) {
            trace_sample_t tmp = s_samples[seq % s_sample_cap];
            if (tmp.session_id == session_id) { *out = tmp; ok = true; }
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool trace_svc_get_event(uint32_t session_id, uint32_t index, trace_event_t *out)
{
    if (!trace_svc_is_ready() || !out) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return false;

    const sess_slot_t *sl = find_slot(session_id);
    if (sl && index < sl->event_count) {
        uint32_t seq = sl->event_first_seq + index;
        if (seq_alive(seq, s_event_seq, s_event_cap)) {
            trace_event_t tmp = s_events[seq % s_event_cap];
            if (tmp.session_id == session_id) { *out = tmp; ok = true; }
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void trace_svc_clear(void)
{
    if (!trace_svc_is_ready()) return;
    if (xSemaphoreTake(s_lock, LOCK_TIMEOUT_READER) != pdTRUE) return;
    memset(s_sessions, 0, sizeof(s_sessions));
    s_sess_head  = 0;
    s_sample_seq = 0;
    s_event_seq  = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "cleared");
}
