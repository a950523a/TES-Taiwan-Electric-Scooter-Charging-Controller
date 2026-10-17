#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include "tes_protocol/tes_types.h"
#include "services/wifi_list.h"

// 充入電量停止條件的範圍（Wh）：0.1–20.0 kWh。網頁、App、OLED 選單與 POST /config 都照這個
#define STOP_ENERGY_MIN_WH  100
#define STOP_ENERGY_MAX_WH  20000

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
    // 清單裡最前面那組（相容用：舊韌體、舊 App 只認得一組）。真正的清單在 config_svc_wifi_get()
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
    uint16_t    stop_energy_wh;      // 充入電量目標（stop_mode=ENERGY 時有效，Wh，100–20000）
    char        notify_url[128];      // Webhook / ntfy URL（空字串 = 停用）
    char        mqtt_broker_url[128]; // MQTT broker URI，e.g. "mqtt://broker.hivemq.com:1883"（空字串 = 停用）
    char        mqtt_topic_prefix[64];// MQTT topic prefix，e.g. "tes/charger"
    // false = 只發佈狀態，不訂閱 {prefix}/cmd。關掉遠端啟停，等於把「任何能對
    // 這個 broker 發佈的人都能操作充電器」這條路封死。預設 true（相容既有使用者）
    bool        mqtt_cmd_enabled;
    bool        sched_enabled;        // 定時充電 master switch
    uint16_t    sched_start_min;      // 開始時間，分鐘數 from midnight 0-1439
    bool        sched_stop_en;        // 自動結束開關
    uint16_t    sched_stop_min;       // 結束時間，分鐘數 from midnight 0-1439
    bool        auto_start;           // Beta: VP 常通 + CAN/CP 邊緣自動觸發充電
    uint8_t     psu_transport;        // 0=UART, 1=ESP-NOW
    uint8_t     psu_peer_mac[6];      // ESP-NOW peer MAC (PSU 的 MAC 地址)
    bool        psu_paired;           // 已完成 ESP-NOW 配對
    // ── OLED 顯示（2026-10-17）────────────────────────────────────────────
    uint8_t     oled_lang;            // OLED_LANG_ZH / OLED_LANG_EN
    uint8_t     oled_anim;            // 充電中動畫：OLED_ANIM_*
    uint8_t     ss_min;               // 螢幕保護等待分鐘：0（關）／5／15／30
    uint8_t     ss_style;             // 螢幕保護樣式：OLED_SS_*
} charger_config_t;

enum { OLED_LANG_ZH = 0, OLED_LANG_EN = 1 };
enum { OLED_ANIM_BOTH = 0, OLED_ANIM_BATTERY = 1, OLED_ANIM_ARROWS = 2, OLED_ANIM_OFF = 3 };
enum { OLED_SS_TURTLE = 0, OLED_SS_DARK = 1 };
#define OLED_SS_MIN_VALID(m) ((m) == 0 || (m) == 5 || (m) == 15 || (m) == 30)

esp_err_t                config_svc_init         (void);
const charger_config_t  *config_svc_get          (void);  // pointer to RAM cache

// 一致性快照：setter 會連續改寫多個欄位，直接透過 config_svc_get() 逐欄位讀取
// 可能讀到「新電壓 + 舊電流」這種混合狀態。任何一次要用到多個欄位、且該組合
// 必須自洽的呼叫端（尤其是 task_tes_sm，它會把值直接放進 0x508 廣播給 BMS）
// 都應該改用這個函式。
void                     config_svc_get_copy     (charger_config_t *out);

esp_err_t config_svc_set_device_name    (const char *name);
esp_err_t config_svc_set_charging       (uint16_t v_01v, uint16_t a_01a, int8_t soc);
// 舊介面：加入／更新一組並排到最前面（pass == NULL 保留原密碼）。空 SSID 不做事
esp_err_t config_svc_set_wifi           (const char *ssid, const char *pass);

// ── 記住的 WiFi（最多 WIFI_LIST_MAX 組，2026-10-17）────────────────────────────
// NVS blob "wifi_nets"；第一組同時寫回舊的 wifi_ssid／wifi_pass，降版韌體還連得上。
// 密碼只在這裡與 network_svc 之間流動，GET /config 只給 SSID。
void      config_svc_wifi_get           (wifi_list_t *out);
esp_err_t config_svc_wifi_add           (const char *ssid, const char *pass);   // INVALID_ARG／NO_MEM（滿了）
esp_err_t config_svc_wifi_remove        (const char *ssid);                      // NOT_FOUND
uint32_t  config_svc_wifi_gen           (void);   // 清單每改一次 +1，network_svc 用它發現變更

// 連不上 WiFi 時自動開的熱點的密碼（WPA2）。第一次開機隨機產生 10 碼，存 NVS "ap_pass"；
// OLED 設定選單「熱點密碼」顯示。首次設定（沒記住任何網路）的熱點仍然不加密。
void      config_svc_get_ap_pass        (char out[WIFI_PASS_MAX_LEN + 1]);
esp_err_t config_svc_set_ap_pass        (const char *pass);   // 8–63 個可見 ASCII，否則 INVALID_ARG
esp_err_t config_svc_set_sta_enabled    (bool enabled);   // false = 固定 AP 模式，保留憑證
esp_err_t config_svc_set_beacon         (bool unlocked);
esp_err_t config_svc_set_auto_voltage   (bool enabled);
esp_err_t config_svc_set_stop          (stop_mode_t mode, uint16_t stop_voltage_01v, uint16_t charge_timer_min);
esp_err_t config_svc_set_stop_energy   (uint16_t wh);   // STOP_ENERGY_MIN_WH–MAX_WH，範圍外回 INVALID_ARG
esp_err_t config_svc_set_notify_url    (const char *url);
esp_err_t config_svc_set_mqtt          (const char *broker_url, const char *topic_prefix);
esp_err_t config_svc_set_mqtt_cmd      (bool enabled);   // false = 只發佈狀態，不訂閱 cmd
esp_err_t config_svc_set_scheduler     (bool enabled, uint16_t start_min, bool stop_en, uint16_t stop_min);
esp_err_t config_svc_set_auto_start    (bool enabled);
esp_err_t config_svc_set_psu           (uint8_t transport, const uint8_t *peer_mac_6, bool paired);
// OLED 語言、充電動畫、螢幕保護。範圍外的值不寫（回 INVALID_ARG）
esp_err_t config_svc_set_oled          (uint8_t lang, uint8_t anim, uint8_t ss_min, uint8_t ss_style);

// ESP-NOW 配對的長期金鑰（32 B）。刻意不放進 charger_config_t：那個結構會整包
// 被 GET /config 送出去，金鑰是秘密。沒有金鑰（從未配對、或舊版只存了 MAC）回傳 false。
bool      config_svc_get_psu_ltk       (uint8_t ltk_32[32]);
// 配對完成：MAC 與金鑰一起寫入，transport 設為 ESP-NOW
esp_err_t config_svc_set_psu_pairing   (const uint8_t peer_mac_6[6], const uint8_t ltk_32[32]);
void      config_svc_override_voltage   (uint16_t v_01v);  // RAM only, no NVS write
// Auto Volt 讀到的電源電壓超過這塊板的上限（＋容差）時記下讀值，0 = 沒超過。只在 RAM。
// 網頁、App、OLED 用它跳警告：電源開太高，這塊板量不準、送給車子的 VLIM2 也比電源低
void      config_svc_set_volt_over      (uint16_t v_01v);
uint16_t  config_svc_volt_over          (void);
