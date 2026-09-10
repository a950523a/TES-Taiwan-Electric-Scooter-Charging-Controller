#include "services/config_svc.h"
#include "hal/hal_nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "config_svc";

#define NVS_NS          "tes_cfg"
#define NVS_KEY_MAX_V   "max_v"
#define NVS_KEY_MAX_A   "max_a"
#define NVS_KEY_SOC     "target_soc"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_BEACON  "beacon"
#define NVS_KEY_AUTO_V  "auto_v"
#define NVS_KEY_STOP_M  "stop_m"
#define NVS_KEY_STOP_V  "stop_v"
#define NVS_KEY_TIMER_M "timer_m"
#define NVS_KEY_NOTIFY     "notify_url"
#define NVS_KEY_MQTT_URL      "mqtt_url"
#define NVS_KEY_MQTT_TOPIC    "mqtt_topic"
#define NVS_KEY_SCHED_EN      "sched_en"
#define NVS_KEY_SCHED_START   "sched_start"
#define NVS_KEY_SCHED_STOP_EN "sched_stop_en"
#define NVS_KEY_SCHED_STOP    "sched_stop"
#define NVS_KEY_AUTO_START    "auto_s"
#define NVS_KEY_PSU_TRANS     "psu_trans"
#define NVS_KEY_PSU_MAC       "psu_mac"
#define NVS_KEY_DEV_NAME      "dev_name"

#define DEFAULT_MAX_V   1000   // 100.0 V
#define DEFAULT_MAX_A   100    // 10.0 A
#define DEFAULT_SOC     80
#define DEFAULT_STOP_V  1000   // 100.0 V
#define DEFAULT_TIMER_M 120    // 120 min

static charger_config_t s_cfg;

// 保護 s_cfg 的多欄位更新。setter 由 HTTP / display task 呼叫，
// 讀取端（task_tes_sm）每 10ms 會取用。臨界區內只有 memcpy，非常短。
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

#define CFG_WRITE_BEGIN()  taskENTER_CRITICAL(&s_cfg_mux)
#define CFG_WRITE_END()    taskEXIT_CRITICAL(&s_cfg_mux)

void config_svc_get_copy(charger_config_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_cfg_mux);
    memcpy(out, &s_cfg, sizeof(*out));
    taskEXIT_CRITICAL(&s_cfg_mux);
}

esp_err_t config_svc_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    // 裝置識別碼：MAC 後 3 bytes。efuse 讀取，不需要 WiFi 已啟動。
    // 同網段兩台機器就是靠這組值區分 mDNS 主機名與 AP SSID。
    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_cfg.device_id, sizeof(s_cfg.device_id),
                 "%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    if (hal_nvs_get_str(NVS_NS, NVS_KEY_DEV_NAME,
                        s_cfg.device_name, sizeof(s_cfg.device_name)) != ESP_OK) {
        s_cfg.device_name[0] = '\0';
    }

    uint32_t tmp;
    s_cfg.max_voltage_01v = (hal_nvs_get_u32(NVS_NS, NVS_KEY_MAX_V, &tmp) == ESP_OK)
                            ? (uint16_t)tmp : DEFAULT_MAX_V;
    s_cfg.max_current_01a = (hal_nvs_get_u32(NVS_NS, NVS_KEY_MAX_A, &tmp) == ESP_OK)
                            ? (uint16_t)tmp : DEFAULT_MAX_A;

    int32_t soc;
    s_cfg.target_soc = (hal_nvs_get_i32(NVS_NS, NVS_KEY_SOC, &soc) == ESP_OK)
                       ? (int8_t)soc : DEFAULT_SOC;

    if (hal_nvs_get_str(NVS_NS, NVS_KEY_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid)) != ESP_OK) {
        s_cfg.wifi_ssid[0] = '\0';
    }
    if (hal_nvs_get_str(NVS_NS, NVS_KEY_PASS, s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass)) != ESP_OK) {
        s_cfg.wifi_pass[0] = '\0';
    }

    bool beacon;
    s_cfg.beacon_unlocked = (hal_nvs_get_bool(NVS_NS, NVS_KEY_BEACON, &beacon) == ESP_OK) && beacon;

    bool auto_v;
    s_cfg.auto_voltage = (hal_nvs_get_bool(NVS_NS, NVS_KEY_AUTO_V, &auto_v) == ESP_OK) && auto_v;

    {
        uint32_t tmp;
        s_cfg.stop_mode = (hal_nvs_get_u32(NVS_NS, NVS_KEY_STOP_M, &tmp) == ESP_OK && tmp <= 2)
                          ? (stop_mode_t)tmp : STOP_MODE_SOC;
        s_cfg.stop_voltage_01v = (hal_nvs_get_u32(NVS_NS, NVS_KEY_STOP_V, &tmp) == ESP_OK)
                                 ? (uint16_t)tmp : DEFAULT_STOP_V;
        s_cfg.charge_timer_min = (hal_nvs_get_u32(NVS_NS, NVS_KEY_TIMER_M, &tmp) == ESP_OK
                                  && tmp >= 1 && tmp <= 600)
                                 ? (uint16_t)tmp : DEFAULT_TIMER_M;
    }

    if (hal_nvs_get_str(NVS_NS, NVS_KEY_NOTIFY, s_cfg.notify_url, sizeof(s_cfg.notify_url)) != ESP_OK) {
        s_cfg.notify_url[0] = '\0';
    }

    if (hal_nvs_get_str(NVS_NS, NVS_KEY_MQTT_URL, s_cfg.mqtt_broker_url, sizeof(s_cfg.mqtt_broker_url)) != ESP_OK) {
        s_cfg.mqtt_broker_url[0] = '\0';
    }
    if (hal_nvs_get_str(NVS_NS, NVS_KEY_MQTT_TOPIC, s_cfg.mqtt_topic_prefix, sizeof(s_cfg.mqtt_topic_prefix)) != ESP_OK) {
        // 預設帶上 device_id：兩台機器共用同一個 broker 時，
        // 舊的固定值 "tes/charger" 會讓兩邊互相覆蓋彼此的狀態與指令。
        // 已經設定過 MQTT 的裝置 NVS 裡有值，不受影響。
        snprintf(s_cfg.mqtt_topic_prefix, sizeof(s_cfg.mqtt_topic_prefix),
                 "tes/%s", s_cfg.device_id);
    }

    {
        bool b;
        uint32_t tmp;
        s_cfg.sched_enabled  = (hal_nvs_get_bool(NVS_NS, NVS_KEY_SCHED_EN,      &b)   == ESP_OK) && b;
        s_cfg.sched_start_min = (hal_nvs_get_u32(NVS_NS, NVS_KEY_SCHED_START,   &tmp) == ESP_OK
                                  && tmp < 1440) ? (uint16_t)tmp : 0;
        s_cfg.sched_stop_en  = (hal_nvs_get_bool(NVS_NS, NVS_KEY_SCHED_STOP_EN, &b)   == ESP_OK) && b;
        s_cfg.sched_stop_min  = (hal_nvs_get_u32(NVS_NS, NVS_KEY_SCHED_STOP,    &tmp) == ESP_OK
                                  && tmp < 1440) ? (uint16_t)tmp : 360;
    }

    {
        bool b;
        s_cfg.auto_start = (hal_nvs_get_bool(NVS_NS, NVS_KEY_AUTO_START, &b) == ESP_OK) && b;
    }

    {
        uint32_t tmp;
        s_cfg.psu_transport = (hal_nvs_get_u32(NVS_NS, NVS_KEY_PSU_TRANS, &tmp) == ESP_OK && tmp <= 1)
                              ? (uint8_t)tmp : 0;
        size_t mac_len = 6;
        s_cfg.psu_paired = (hal_nvs_get_blob(NVS_NS, NVS_KEY_PSU_MAC,
                                             s_cfg.psu_peer_mac, &mac_len) == ESP_OK && mac_len == 6);
    }

    ESP_LOGI(TAG, "device: id=%s name=\"%s\"", s_cfg.device_id, s_cfg.device_name);
    ESP_LOGI(TAG, "loaded: V=%u A=%u SOC=%d beacon=%d auto_v=%d stop=%d stpV=%u timer=%u",
             s_cfg.max_voltage_01v, s_cfg.max_current_01a, s_cfg.target_soc,
             (int)s_cfg.beacon_unlocked, (int)s_cfg.auto_voltage,
             (int)s_cfg.stop_mode, s_cfg.stop_voltage_01v, s_cfg.charge_timer_min);
    return ESP_OK;
}

const charger_config_t *config_svc_get(void)
{
    return &s_cfg;
}

esp_err_t config_svc_set_device_name(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    CFG_WRITE_BEGIN();
    strncpy(s_cfg.device_name, name, sizeof(s_cfg.device_name) - 1);
    s_cfg.device_name[sizeof(s_cfg.device_name) - 1] = '\0';
    CFG_WRITE_END();
    return hal_nvs_set_str(NVS_NS, NVS_KEY_DEV_NAME, s_cfg.device_name);
}

esp_err_t config_svc_set_charging(uint16_t v_01v, uint16_t a_01a, int8_t soc)
{
    CFG_WRITE_BEGIN();
    s_cfg.max_voltage_01v = v_01v;
    s_cfg.max_current_01a = a_01a;
    s_cfg.target_soc      = soc;
    CFG_WRITE_END();
    esp_err_t r = hal_nvs_set_u32(NVS_NS, NVS_KEY_MAX_V, v_01v);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_MAX_A, a_01a);
    r |= hal_nvs_set_i32(NVS_NS, NVS_KEY_SOC, soc);
    return r;
}

esp_err_t config_svc_set_wifi(const char *ssid, const char *pass)
{
    CFG_WRITE_BEGIN();
    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';
    strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
    s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    CFG_WRITE_END();
    esp_err_t r = hal_nvs_set_str(NVS_NS, NVS_KEY_SSID, s_cfg.wifi_ssid);
    r |= hal_nvs_set_str(NVS_NS, NVS_KEY_PASS, s_cfg.wifi_pass);
    return r;
}

esp_err_t config_svc_set_beacon(bool unlocked)
{
    s_cfg.beacon_unlocked = unlocked;
    return hal_nvs_set_bool(NVS_NS, NVS_KEY_BEACON, unlocked);
}

esp_err_t config_svc_set_auto_voltage(bool enabled)
{
    s_cfg.auto_voltage = enabled;
    return hal_nvs_set_bool(NVS_NS, NVS_KEY_AUTO_V, enabled);
}

esp_err_t config_svc_set_stop(stop_mode_t mode, uint16_t stop_voltage_01v, uint16_t charge_timer_min)
{
    CFG_WRITE_BEGIN();
    s_cfg.stop_mode        = mode;
    s_cfg.stop_voltage_01v = stop_voltage_01v;
    s_cfg.charge_timer_min = charge_timer_min;
    CFG_WRITE_END();
    esp_err_t r = hal_nvs_set_u32(NVS_NS, NVS_KEY_STOP_M, (uint32_t)mode);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_STOP_V, stop_voltage_01v);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_TIMER_M, charge_timer_min);
    return r;
}

esp_err_t config_svc_set_notify_url(const char *url)
{
    CFG_WRITE_BEGIN();
    strncpy(s_cfg.notify_url, url, sizeof(s_cfg.notify_url) - 1);
    s_cfg.notify_url[sizeof(s_cfg.notify_url) - 1] = '\0';
    CFG_WRITE_END();
    return hal_nvs_set_str(NVS_NS, NVS_KEY_NOTIFY, s_cfg.notify_url);
}

esp_err_t config_svc_set_mqtt(const char *broker_url, const char *topic_prefix)
{
    CFG_WRITE_BEGIN();
    strncpy(s_cfg.mqtt_broker_url,   broker_url,    sizeof(s_cfg.mqtt_broker_url) - 1);
    strncpy(s_cfg.mqtt_topic_prefix, topic_prefix,  sizeof(s_cfg.mqtt_topic_prefix) - 1);
    s_cfg.mqtt_broker_url[sizeof(s_cfg.mqtt_broker_url) - 1]     = '\0';
    s_cfg.mqtt_topic_prefix[sizeof(s_cfg.mqtt_topic_prefix) - 1] = '\0';
    CFG_WRITE_END();
    esp_err_t r = hal_nvs_set_str(NVS_NS, NVS_KEY_MQTT_URL,   s_cfg.mqtt_broker_url);
    r |= hal_nvs_set_str(NVS_NS, NVS_KEY_MQTT_TOPIC, s_cfg.mqtt_topic_prefix);
    return r;
}

esp_err_t config_svc_set_scheduler(bool enabled, uint16_t start_min, bool stop_en, uint16_t stop_min)
{
    CFG_WRITE_BEGIN();
    s_cfg.sched_enabled   = enabled;
    s_cfg.sched_start_min = start_min;
    s_cfg.sched_stop_en   = stop_en;
    s_cfg.sched_stop_min  = stop_min;
    CFG_WRITE_END();
    esp_err_t r = hal_nvs_set_bool(NVS_NS, NVS_KEY_SCHED_EN,      enabled);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_SCHED_START,   start_min);
    r |= hal_nvs_set_bool(NVS_NS, NVS_KEY_SCHED_STOP_EN, stop_en);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_SCHED_STOP,    stop_min);
    return r;
}

esp_err_t config_svc_set_auto_start(bool enabled)
{
    s_cfg.auto_start = enabled;
    return hal_nvs_set_bool(NVS_NS, NVS_KEY_AUTO_START, enabled);
}

esp_err_t config_svc_set_psu(uint8_t transport, const uint8_t *peer_mac_6, bool paired)
{
    uint8_t mac[6];
    if (peer_mac_6) memcpy(mac, peer_mac_6, 6);   // 呼叫端可能傳入 &s_cfg.psu_peer_mac 本身

    CFG_WRITE_BEGIN();
    s_cfg.psu_transport = transport;
    s_cfg.psu_paired    = paired;
    if (peer_mac_6) memcpy(s_cfg.psu_peer_mac, mac, 6);
    CFG_WRITE_END();

    esp_err_t r = hal_nvs_set_u32(NVS_NS, NVS_KEY_PSU_TRANS, transport);
    if (peer_mac_6) r |= hal_nvs_set_blob(NVS_NS, NVS_KEY_PSU_MAC, mac, 6);
    return r;
}

void config_svc_override_voltage(uint16_t v_01v)
{
    s_cfg.max_voltage_01v = v_01v;
    ESP_LOGI(TAG, "auto-voltage: max_v overridden to %u.%uV", v_01v / 10, v_01v % 10);
}
