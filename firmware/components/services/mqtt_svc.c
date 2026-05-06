#include "services/mqtt_svc.h"
#include "services/event_bus.h"
#include "services/config_svc.h"
#include "tes_protocol/tes_sm.h"
#include "tes_protocol/tes_types.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>

// IPC objects owned by main.c
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;
extern QueueHandle_t     g_btn_event_queue;

static const char *TAG = "mqtt_svc";

static esp_mqtt_client_handle_t s_client         = NULL;
static atomic_bool              s_mqtt_connected  = ATOMIC_VAR_INIT(false);

// Topic buffers — written once at task start, read by event handler (both static)
static char s_topic_status[176];
static char s_topic_cmd[176];

bool mqtt_svc_is_connected(void)
{
    return atomic_load(&s_mqtt_connected);
}

static const char *state_str(tes_state_t s)
{
    switch (s) {
    case TES_STATE_IDLE:           return "idle";
    case TES_STATE_PARAM_EXCHANGE: return "connecting";
    case TES_STATE_PRE_CHARGE:     return "pre_charge";
    case TES_STATE_CHARGING:       return "charging";
    case TES_STATE_ENDING:         return "ending";
    case TES_STATE_FAULT:          return "fault";
    case TES_STATE_EMERGENCY:      return "emergency";
    case TES_STATE_FINALIZE:       return "finalizing";
    default:                       return "unknown";
    }
}

static int build_status_json(char *buf, size_t len)
{
    tes_snapshot_t snap = {0};
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snap = g_snapshot;
        xSemaphoreGive(g_snapshot_mutex);
    }
    return snprintf(buf, len,
        "{\"state\":\"%s\",\"voltage\":%.1f,\"current\":%.1f,"
        "\"soc\":%d,\"target_soc\":%d,"
        "\"elapsed_seconds\":%lu,\"remaining_seconds\":%lu,"
        "\"fault\":%s,\"charge_complete\":%s,\"timer_running\":%s}",
        state_str(snap.state),
        snap.output_voltage, snap.output_current,
        (int)snap.soc, (int)snap.target_soc,
        (unsigned long)snap.elapsed_seconds,
        (unsigned long)snap.remaining_seconds,
        snap.fault_latched   ? "true" : "false",
        snap.charge_complete ? "true" : "false",
        snap.timer_running   ? "true" : "false");
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t evt = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        atomic_store(&s_mqtt_connected, true);
        esp_mqtt_client_subscribe(evt->client, s_topic_cmd, 1);
        {
            char buf[384];
            build_status_json(buf, sizeof(buf));
            esp_mqtt_client_publish(evt->client, s_topic_status, buf, 0, 0, 1);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected from broker");
        atomic_store(&s_mqtt_connected, false);
        break;

    case MQTT_EVENT_DATA:
        if (evt->data_len > 0 && evt->topic_len > 0) {
            if ((int)strlen(s_topic_cmd) == evt->topic_len &&
                memcmp(s_topic_cmd, evt->topic, evt->topic_len) == 0)
            {
                char data[64] = {0};
                int n = evt->data_len < (int)(sizeof(data) - 1)
                        ? evt->data_len : (int)(sizeof(data) - 1);
                memcpy(data, evt->data, n);

                uint8_t btn = 0xFF;
                if      (strstr(data, "\"start\"")) btn = EVT_BUTTON_START;
                else if (strstr(data, "\"stop\""))  btn = EVT_BUTTON_STOP;

                if (btn != 0xFF) {
                    xQueueSend(g_btn_event_queue, &btn, 0);
                    ESP_LOGI(TAG, "remote cmd: %s",
                             btn == EVT_BUTTON_START ? "start" : "stop");
                }
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error event");
        break;

    default:
        break;
    }
}

esp_err_t mqtt_svc_init(void)
{
    return ESP_OK;
}

void task_mqtt(void *arg)
{
    (void)arg;

    const charger_config_t *cfg = config_svc_get();
    if (cfg->mqtt_broker_url[0] == '\0') {
        ESP_LOGI(TAG, "not configured, task exiting");
        vTaskDelete(NULL);
        return;
    }

    snprintf(s_topic_status, sizeof(s_topic_status),
             "%s/status", cfg->mqtt_topic_prefix);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd),
             "%s/cmd", cfg->mqtt_topic_prefix);

    static const char lwt_msg[] = "{\"state\":\"offline\"}";

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri          = cfg->mqtt_broker_url,
        .session.last_will.topic     = s_topic_status,
        .session.last_will.msg       = lwt_msg,
        .session.last_will.msg_len   = sizeof(lwt_msg) - 1,
        .session.last_will.qos       = 0,
        .session.last_will.retain    = 1,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client_init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    QueueHandle_t eq = event_bus_subscribe();
    charger_event_t evt;
    tes_state_t last_state = TES_STATE_IDLE;

    for (;;) {
        // Publish more frequently during charging
        TickType_t timeout = (last_state == TES_STATE_CHARGING)
                             ? pdMS_TO_TICKS(10000)
                             : pdMS_TO_TICKS(30000);

        bool got_evt = xQueueReceive(eq, &evt, timeout) == pdTRUE;

        if (got_evt && evt.type == EVT_TES_STATE_CHANGED) {
            last_state = (tes_state_t)evt.payload[0];
        }

        if (!atomic_load(&s_mqtt_connected)) continue;

        char buf[384];
        build_status_json(buf, sizeof(buf));
        esp_mqtt_client_publish(s_client, s_topic_status, buf, 0, 0, 1);
    }
}
