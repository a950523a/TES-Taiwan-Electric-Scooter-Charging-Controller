#include "services/log_svc.h"
#include "services/event_bus.h"
#include "hal/hal_nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define NVS_NS   "tes_hist"
#define NVS_KEY  "log"
#define LOG_MAX  20

static const char *TAG = "log_svc";

// NVS circular buffer: head = index of oldest entry, count = valid entries
typedef struct {
    uint8_t          head;
    uint8_t          count;
    uint8_t          _pad[2];          // align sessions[] to 4 bytes
    charge_session_t sessions[LOG_MAX];
} session_log_t;                       // 4 + 20*12 = 244 bytes

static session_log_t s_log;

esp_err_t log_svc_init(void)
{
    size_t len = sizeof(s_log);
    if (hal_nvs_get_blob(NVS_NS, NVS_KEY, &s_log, &len) != ESP_OK ||
        len != sizeof(s_log)) {
        memset(&s_log, 0, sizeof(s_log));
        ESP_LOGI(TAG, "no history found, starting fresh");
    } else {
        ESP_LOGI(TAG, "loaded %d session(s)", s_log.count);
    }
    return ESP_OK;
}

uint8_t log_svc_get_history(charge_session_t *buf, uint8_t max)
{
    uint8_t n = s_log.count < max ? s_log.count : max;
    for (uint8_t i = 0; i < n; i++) {
        // newest-first: (head + count - 1 - i) % LOG_MAX
        uint8_t idx = (s_log.head + s_log.count - 1 - i) % LOG_MAX;
        buf[i] = s_log.sessions[idx];
    }
    return n;
}

static void save_session(const charge_session_t *sess)
{
    uint8_t idx = (s_log.head + s_log.count) % LOG_MAX;
    s_log.sessions[idx] = *sess;
    if (s_log.count < LOG_MAX) {
        s_log.count++;
    } else {
        s_log.head = (s_log.head + 1) % LOG_MAX;  // overwrite oldest
    }
    esp_err_t err = hal_nvs_set_blob(NVS_NS, NVS_KEY, &s_log, sizeof(s_log));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS write failed: %s", esp_err_to_name(err));
    }
}

void task_log(void *arg)
{
    (void)arg;
    QueueHandle_t q = event_bus_subscribe();
    charger_event_t evt;

    for (;;) {
        if (xQueueReceive(q, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.type != EVT_SESSION_COMPLETE) continue;

        charge_session_t sess;
        memcpy(&sess, evt.payload, sizeof(charge_session_t));
        save_session(&sess);
        ESP_LOGI(TAG, "saved: %us %.2fWh SOC%d->%d reason=%d",
                 sess.duration_s, sess.energy_wh,
                 sess.soc_start, sess.soc_end, sess.stop_reason);
    }
}
