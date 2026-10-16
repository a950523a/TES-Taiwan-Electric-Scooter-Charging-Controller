#include "services/log_svc.h"
#include "services/event_bus.h"
#include "hal/hal_nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

#define NVS_NS     "tes_hist"
#define NVS_KEY    "log"
#define NVS_ENERGY "energy"
#define NVS_EFF    "eff"
#define NVS_PRICE  "price"
#define LOG_MAX    20

// 由 main.c 提供：自行結束的任務要在 vTaskDelete 前註銷 handle，
// 否則 task_monitor 會讀到懸空指標
extern void g_task_unregister_self(void);

static const char *TAG = "log_svc";

_Static_assert(sizeof(session_event_t) <= sizeof(((charger_event_t *)0)->payload),
               "session_event_t exceeds charger_event_t.payload");

// NVS circular buffer: head = index of oldest entry, count = valid entries
typedef struct {
    uint8_t         head;
    uint8_t         count;
    uint8_t         _pad[2];          // align sessions[] to 4 bytes
    charge_record_t sessions[LOG_MAX];
} session_log_t;                      // 4 + 20*28 = 564 bytes

// 2026-10-17 之前的格式：charge_session_t 本身（24 bytes），沒有開始時間。
// 開機讀到這個長度就轉成新格式、開始時間留 0 —— 以前改結構是直接清空紀錄，這次 OTA 後要留著
typedef struct {
    uint8_t          head;
    uint8_t          count;
    uint8_t          _pad[2];
    charge_session_t sessions[LOG_MAX];
} session_log_v1_t;                   // 484 bytes

// 結束時還沒對時的那幾輪：這次開機之內對到時就補上開始時間、算進月份。
// 重開機就沒了，那幾輪留在 undated
typedef struct {
    uint32_t session_id;
    uint32_t start_uptime_ms;
} pending_t;

static session_log_t     s_log;
static session_log_v1_t  s_old;       // 只有開機轉換格式時用，放 .bss 不佔 app_main 的堆疊
static energy_book_t     s_energy;
static pending_t         s_pending[LOG_MAX];
static uint8_t           s_pending_n;
static uint8_t           s_eff   = 90;
static uint16_t          s_price = 0;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static uint32_t now_epoch_s(void)
{
    time_t now = time(NULL);
    return (now > 1600000000) ? (uint32_t)now : 0u;   // 同 trace_svc：之前視為還沒對時
}

esp_err_t log_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    size_t len = sizeof(s_log);
    if (hal_nvs_get_blob(NVS_NS, NVS_KEY, &s_log, &len) == ESP_OK && len == sizeof(s_log)) {
        ESP_LOGI(TAG, "loaded %d session(s)", s_log.count);
    } else {
        memset(&s_log, 0, sizeof(s_log));
        len = sizeof(s_old);
        if (hal_nvs_get_blob(NVS_NS, NVS_KEY, &s_old, &len) == ESP_OK && len == sizeof(s_old) &&
            s_old.count <= LOG_MAX && s_old.head < LOG_MAX) {
            s_log.head  = s_old.head;
            s_log.count = s_old.count;
            for (int i = 0; i < LOG_MAX; i++) s_log.sessions[i].s = s_old.sessions[i];
            hal_nvs_set_blob(NVS_NS, NVS_KEY, &s_log, sizeof(s_log));
            ESP_LOGI(TAG, "migrated %d session(s) to the dated format", s_log.count);
        } else {
            ESP_LOGI(TAG, "no history found, starting fresh");
        }
    }
    if (s_log.count > LOG_MAX || s_log.head >= LOG_MAX) memset(&s_log, 0, sizeof(s_log));

    len = sizeof(s_energy);
    if (hal_nvs_get_blob(NVS_NS, NVS_ENERGY, &s_energy, &len) != ESP_OK ||
        len != sizeof(s_energy) || s_energy.count > ENERGY_MONTHS) {
        memset(&s_energy, 0, sizeof(s_energy));
    }

    uint32_t v;
    if (hal_nvs_get_u32(NVS_NS, NVS_EFF, &v) == ESP_OK && v >= 50 && v <= 100) s_eff = (uint8_t)v;
    if (hal_nvs_get_u32(NVS_NS, NVS_PRICE, &v) == ESP_OK && v <= 2000) s_price = (uint16_t)v;
    return ESP_OK;
}

uint8_t log_svc_get_history(charge_record_t *buf, uint8_t max)
{
    LOCK();
    uint8_t n = s_log.count < max ? s_log.count : max;
    for (uint8_t i = 0; i < n; i++) {
        // newest-first: (head + count - 1 - i) % LOG_MAX
        uint8_t idx = (s_log.head + s_log.count - 1 - i) % LOG_MAX;
        buf[i] = s_log.sessions[idx];
    }
    UNLOCK();
    return n;
}

void log_svc_get_energy(energy_book_t *out)
{
    LOCK();
    *out = s_energy;
    UNLOCK();
}

uint8_t  log_svc_psu_eff_pct(void) { return s_eff; }
uint16_t log_svc_kwh_price_c(void) { return s_price; }

void log_svc_set_cost(uint8_t eff_pct, uint16_t price_c)
{
    if (eff_pct >= 50 && eff_pct <= 100 && eff_pct != s_eff) {
        s_eff = eff_pct;
        hal_nvs_set_u32(NVS_NS, NVS_EFF, eff_pct);
    }
    if (price_c <= 2000 && price_c != s_price) {
        s_price = price_c;
        hal_nvs_set_u32(NVS_NS, NVS_PRICE, price_c);
    }
}

static void save_all(void)
{
    esp_err_t err = hal_nvs_set_blob(NVS_NS, NVS_KEY, &s_log, sizeof(s_log));
    if (err == ESP_OK) err = hal_nvs_set_blob(NVS_NS, NVS_ENERGY, &s_energy, sizeof(s_energy));
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS write failed: %s", esp_err_to_name(err));
}

static void save_session(const session_event_t *ev)
{
    LOCK();
    uint8_t idx = (s_log.head + s_log.count) % LOG_MAX;
    // 被覆蓋的那筆如果還在等對時，就不用等了（它已經不在紀錄裡）
    uint32_t gone = s_log.count == LOG_MAX ? s_log.sessions[idx].s.session_id : 0;
    s_log.sessions[idx] = ev->rec;
    if (s_log.count < LOG_MAX) {
        s_log.count++;
    } else {
        s_log.head = (s_log.head + 1) % LOG_MAX;  // overwrite oldest
    }
    if (ev->rec.start_epoch) {
        energy_book_add(&s_energy, ev->rec.start_epoch, ev->rec.s.energy_wh,
                        ev->rec.s.energy_estimated);
    } else {
        energy_book_add(&s_energy, 0, 0.0f, false);   // undated++，對到時再扣回來
        for (int i = 0; i < s_pending_n; i++) {
            if (gone && s_pending[i].session_id == gone) s_pending[i--] = s_pending[--s_pending_n];
        }
        if (s_pending_n < LOG_MAX) {
            s_pending[s_pending_n].session_id      = ev->rec.s.session_id;
            s_pending[s_pending_n].start_uptime_ms = ev->start_uptime_ms;
            s_pending_n++;
        }
    }
    save_all();
    UNLOCK();
}

// 對到時之後，把這次開機裡沒有開始時間的紀錄補上
static void backfill_pending(void)
{
    if (s_pending_n == 0) return;
    uint32_t now = now_epoch_s();
    if (!now) return;
    uint32_t up_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    LOCK();
    int dated = 0;
    for (int i = 0; i < s_pending_n; i++) {
        uint32_t epoch = now - (up_ms - s_pending[i].start_uptime_ms) / 1000u;
        for (int j = 0; j < LOG_MAX; j++) {
            charge_record_t *r = &s_log.sessions[j];
            if (r->start_epoch == 0 && r->s.session_id == s_pending[i].session_id) {
                r->start_epoch = epoch;
                if (s_energy.undated) s_energy.undated--;
                energy_book_add(&s_energy, epoch, r->s.energy_wh, r->s.energy_estimated);
                dated++;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "clock set: dated %d session(s)", dated);
    s_pending_n = 0;
    save_all();
    UNLOCK();
}

void task_log(void *arg)
{
    (void)arg;
    QueueHandle_t q = event_bus_subscribe();
    if (!q) {
        ESP_LOGE(TAG, "event_bus_subscribe failed — charge history disabled");
        g_task_unregister_self();
        vTaskDelete(NULL);
        return;
    }
    charger_event_t evt;

    for (;;) {
        // 30 秒醒一次看有沒有對到時（沒有待補的紀錄時什麼都不做）
        bool got = xQueueReceive(q, &evt, pdMS_TO_TICKS(30000)) == pdTRUE;
        backfill_pending();
        if (!got || evt.type != EVT_SESSION_COMPLETE) continue;

        session_event_t ev;
        memcpy(&ev, evt.payload, sizeof ev);
        save_session(&ev);
        ESP_LOGI(TAG, "saved: %us %.2fWh SOC%d->%d reason=%d start=%lu",
                 (unsigned)ev.rec.s.duration_s, ev.rec.s.energy_wh,
                 ev.rec.s.soc_start, ev.rec.s.soc_end, ev.rec.s.stop_reason,
                 (unsigned long)ev.rec.start_epoch);
    }
}
