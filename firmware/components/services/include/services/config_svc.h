#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

// Centralised NVS access with RAM cache.
// Fixes V2's hal_update_leds() opening NVS every 50ms.
// All other components call config_svc_get() to read from RAM.

typedef struct {
    uint16_t max_voltage_01v;    // e.g. 1000 = 100.0 V
    uint16_t max_current_01a;    // e.g.  100 =  10.0 A
    int8_t   target_soc;         // 0-100 %
    char     wifi_ssid[33];
    char     wifi_pass[64];
    bool     beacon_unlocked;
    bool     auto_voltage;       // true = 依 BMS max_charge_voltage 自動設定電壓
} charger_config_t;

esp_err_t                config_svc_init         (void);
const charger_config_t  *config_svc_get          (void);  // pointer to RAM cache

esp_err_t config_svc_set_charging    (uint16_t v_01v, uint16_t a_01a, int8_t soc);
esp_err_t config_svc_set_wifi        (const char *ssid, const char *pass);
esp_err_t config_svc_set_beacon      (bool unlocked);
esp_err_t config_svc_set_auto_voltage(bool enabled);
