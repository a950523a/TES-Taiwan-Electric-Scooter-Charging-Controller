#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include "tes_protocol/tes_types.h"

// Centralised NVS access with RAM cache.
// Fixes V2's hal_update_leds() opening NVS every 50ms.
// All other components call config_svc_get() to read from RAM.

typedef struct {
    uint16_t max_voltage_01v;    // e.g. 1000 = 100.0 V
    uint16_t max_current_01a;    // e.g.  100 =  10.0 A
    int8_t   target_soc;         // 0-100 %
    char     wifi_ssid[33];
    char     wifi_pass[64];
    bool        beacon_unlocked;
    bool        auto_voltage;        // true = 依 ADC 自動設定電壓（開機時讀一次）
    stop_mode_t stop_mode;           // STOP_MODE_SOC / STOP_MODE_VOLTAGE / STOP_MODE_TIMER
    uint16_t    stop_voltage_01v;    // 充電停止電壓（stop_mode=VOLTAGE 時有效）
    uint16_t    charge_timer_min;    // 充電時長上限（stop_mode=TIMER 時有效，分鐘）
    char        notify_url[128];      // Webhook / ntfy URL（空字串 = 停用）
    char        mqtt_broker_url[128]; // MQTT broker URI，e.g. "mqtt://broker.hivemq.com:1883"（空字串 = 停用）
    char        mqtt_topic_prefix[64];// MQTT topic prefix，e.g. "tes/charger"
    bool        sched_enabled;        // 定時充電 master switch
    uint16_t    sched_start_min;      // 開始時間，分鐘數 from midnight 0-1439
    bool        sched_stop_en;        // 自動結束開關
    uint16_t    sched_stop_min;       // 結束時間，分鐘數 from midnight 0-1439
    bool        auto_start;           // Beta: VP 常通 + CAN/CP 邊緣自動觸發充電
    uint8_t     psu_transport;        // 0=UART, 1=ESP-NOW
    uint8_t     psu_peer_mac[6];      // ESP-NOW peer MAC (PSU 的 MAC 地址)
    bool        psu_paired;           // 已完成 ESP-NOW 配對
} charger_config_t;

esp_err_t                config_svc_init         (void);
const charger_config_t  *config_svc_get          (void);  // pointer to RAM cache

esp_err_t config_svc_set_charging       (uint16_t v_01v, uint16_t a_01a, int8_t soc);
esp_err_t config_svc_set_wifi           (const char *ssid, const char *pass);
esp_err_t config_svc_set_beacon         (bool unlocked);
esp_err_t config_svc_set_auto_voltage   (bool enabled);
esp_err_t config_svc_set_stop          (stop_mode_t mode, uint16_t stop_voltage_01v, uint16_t charge_timer_min);
esp_err_t config_svc_set_notify_url    (const char *url);
esp_err_t config_svc_set_mqtt          (const char *broker_url, const char *topic_prefix);
esp_err_t config_svc_set_scheduler     (bool enabled, uint16_t start_min, bool stop_en, uint16_t stop_min);
esp_err_t config_svc_set_auto_start    (bool enabled);
esp_err_t config_svc_set_psu           (uint8_t transport, const uint8_t *peer_mac_6, bool paired);
void      config_svc_override_voltage   (uint16_t v_01v);  // RAM only, no NVS write
