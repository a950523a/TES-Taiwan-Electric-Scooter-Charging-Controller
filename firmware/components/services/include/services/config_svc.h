#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include "tes_protocol/tes_types.h"

// Centralised NVS access with RAM cache.
// Fixes V2's hal_update_leds() opening NVS every 50ms.
// All other components call config_svc_get() to read from RAM.

typedef struct {
    // ── 裝置identity（多台同網段時用來區分）────────────────────────────────
    // device_id 由 MAC 後 3 bytes 產生，唯讀、不可改，保證同網段唯一。
    // device_name 是使用者自訂的顯示名稱，可留空。
    char     device_id[7];       // "a1b2c3"（小寫 hex，6 字 + NUL）
    char     device_name[25];    // 使用者自訂，空字串 = 未命名

    uint16_t max_voltage_01v;    // e.g. 1000 = 100.0 V
    uint16_t max_current_01a;    // e.g.  100 =  10.0 A
    int8_t   target_soc;         // 0-100 %
    char     wifi_ssid[33];
    char     wifi_pass[64];
    // false = 不連線至路由器，固定開熱點，但 SSID／密碼保留不清除。
    // 沒有這個開關的話，想回到 AP 模式只能把 SSID 清空，連帶弄丟密碼。
    bool     sta_enabled;
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

// 一致性快照：setter 會連續改寫多個欄位，直接透過 config_svc_get() 逐欄位讀取
// 可能讀到「新電壓 + 舊電流」這種混合狀態。任何一次要用到多個欄位、且該組合
// 必須自洽的呼叫端（尤其是 task_tes_sm，它會把值直接放進 0x508 廣播給 BMS）
// 都應該改用這個函式。
void                     config_svc_get_copy     (charger_config_t *out);

esp_err_t config_svc_set_device_name    (const char *name);
esp_err_t config_svc_set_charging       (uint16_t v_01v, uint16_t a_01a, int8_t soc);
esp_err_t config_svc_set_wifi           (const char *ssid, const char *pass);
esp_err_t config_svc_set_sta_enabled    (bool enabled);   // false = 固定 AP 模式，保留憑證
esp_err_t config_svc_set_beacon         (bool unlocked);
esp_err_t config_svc_set_auto_voltage   (bool enabled);
esp_err_t config_svc_set_stop          (stop_mode_t mode, uint16_t stop_voltage_01v, uint16_t charge_timer_min);
esp_err_t config_svc_set_notify_url    (const char *url);
esp_err_t config_svc_set_mqtt          (const char *broker_url, const char *topic_prefix);
esp_err_t config_svc_set_scheduler     (bool enabled, uint16_t start_min, bool stop_en, uint16_t stop_min);
esp_err_t config_svc_set_auto_start    (bool enabled);
esp_err_t config_svc_set_psu           (uint8_t transport, const uint8_t *peer_mac_6, bool paired);
void      config_svc_override_voltage   (uint16_t v_01v);  // RAM only, no NVS write
