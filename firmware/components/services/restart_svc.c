// restart_svc.c — 見 restart_svc.h
#include "services/restart_svc.h"
#include "tes_protocol/tes_sm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"

// IPC objects owned by main.c — extern, same pattern as display_svc.c / network_svc.c
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;

static const char *TAG = "restart_svc";
static volatile bool s_pending;

bool restart_svc_allowed(void)
{
    tes_state_t st;
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;   // 讀不到狀態 → 當作忙碌
    }
    st = g_snapshot.state;
    xSemaphoreGive(g_snapshot_mutex);
    return !(st == TES_STATE_PARAM_EXCHANGE || st == TES_STATE_PRE_CHARGE ||
             st == TES_STATE_CHARGING       || st == TES_STATE_ENDING);
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

bool restart_svc_request(uint32_t delay_ms, const char *who)
{
    if (s_pending) return true;
    if (!restart_svc_allowed()) {
        ESP_LOGW(TAG, "restart from %s refused: charging in progress", who);
        return false;
    }
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) != ESP_OK ||
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000u) != ESP_OK) {
        ESP_LOGE(TAG, "restart timer failed — restarting now");
        esp_restart();
    }
    s_pending = true;
    ESP_LOGW(TAG, "restart requested from %s, in %lu ms", who, (unsigned long)delay_ms);
    return true;
}

bool restart_svc_pending(void)
{
    return s_pending;
}
