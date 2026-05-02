#include "services/config_svc.h"
#include "hal/hal_nvs.h"
#include "esp_log.h"
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

#define DEFAULT_MAX_V   1000   // 100.0 V
#define DEFAULT_MAX_A   100    // 10.0 A
#define DEFAULT_SOC     80

static charger_config_t s_cfg;

esp_err_t config_svc_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

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

    ESP_LOGI(TAG, "loaded: V=%u A=%u SOC=%d beacon=%d auto_v=%d",
             s_cfg.max_voltage_01v, s_cfg.max_current_01a,
             s_cfg.target_soc, (int)s_cfg.beacon_unlocked, (int)s_cfg.auto_voltage);
    return ESP_OK;
}

const charger_config_t *config_svc_get(void)
{
    return &s_cfg;
}

esp_err_t config_svc_set_charging(uint16_t v_01v, uint16_t a_01a, int8_t soc)
{
    s_cfg.max_voltage_01v = v_01v;
    s_cfg.max_current_01a = a_01a;
    s_cfg.target_soc      = soc;
    esp_err_t r = hal_nvs_set_u32(NVS_NS, NVS_KEY_MAX_V, v_01v);
    r |= hal_nvs_set_u32(NVS_NS, NVS_KEY_MAX_A, a_01a);
    r |= hal_nvs_set_i32(NVS_NS, NVS_KEY_SOC, soc);
    return r;
}

esp_err_t config_svc_set_wifi(const char *ssid, const char *pass)
{
    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
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
