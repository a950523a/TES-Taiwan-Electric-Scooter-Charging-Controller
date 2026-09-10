// network_svc.c — WiFi + REST API
//
// No SSID configured → AP mode, broadcasts "TES-Charger-<id>" (open), IP 192.168.4.1
// SSID configured    → STA mode, auto-reconnect
// HTTP server runs in both modes on port 80.
//
// ── 同一個網段放兩台機器 ──────────────────────────────────────────────────────
// 舊版把 mDNS 主機名 "tes-charger" 和 AP SSID "TES-Charger" 都寫死，兩台機器
// 會同時搶同一個名字，誰也連不準。現在每台都用 MAC 後 3 bytes 當 device_id：
//
//   主機名   tes-<id>.local        永遠唯一，改名字也不會變（可以放書籤）
//   AP SSID  TES-Charger-<id>      設定模式下兩台才分得出來
//   服務名稱 使用者自訂的 device_name，給 Bonjour / 服務瀏覽器看的
//   TXT      id / name / ver，未來 App 掃描區網時可直接辨識
//
// 另外用 delegated hostname 額外掛一個 "tes-charger"，讓單機使用者原本的
// tes-charger.local 繼續能用；兩台都掛時會由其中一台回應，但兩台各自的
// tes-<id>.local 一定準確，所以不影響。

#include "services/network_svc.h"
#include "services/config_svc.h"
#include "drivers/psu_driver.h"
#include "services/event_bus.h"
#include "services/ota_svc.h"
#include "services/notify_svc.h"
#include "services/log_svc.h"
#include "services/trace_svc.h"
#include "services/scheduler_svc.h"
#include "tes_protocol/tes_types.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "network_svc";

// IPC objects owned by main.c — extern, same pattern as display_svc.c
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;
extern QueueHandle_t     g_btn_event_queue;

static httpd_handle_t s_server    = NULL;
static bool           s_connected = false;
static bool           s_ap_mode   = false;
static bool           s_mdns_ok   = false;
static char           s_ip_str[20] = "";   // "" = not yet known
static char           s_hostname[24] = ""; // "tes-<id>"，唯一
static char           s_ap_ssid[32]  = ""; // "TES-Charger-<id>"

static bool           s_legacy_svc_ok = false;

#define LEGACY_HOSTNAME "tes-charger"      // 相容用的別名，見檔頭說明
#define MDNS_DEV_MARKER "tes-charger"      // mDNS TXT "dev=" 的值，用來辨識本產品

// 沒取名字時顯示的名稱
static void friendly_name(const charger_config_t *cfg, char *out, size_t len)
{
    if (cfg->device_name[0]) snprintf(out, len, "%s", cfg->device_name);
    else                     snprintf(out, len, "TES Charger %s", cfg->device_id);
}

void network_svc_get_hostname(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    snprintf(buf, len, "%s", s_hostname);
}

#define MAX_BODY 512

// ── Public state getters ──────────────────────────────────────────────────────

bool network_svc_is_ap_mode(void)
{
    return s_ap_mode;
}

void network_svc_get_ip_str(char *buf, size_t len)
{
    strncpy(buf, s_ip_str, len - 1);
    buf[len - 1] = '\0';
}

bool network_svc_is_connected(void)
{
    return s_connected;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static const char *state_name(tes_state_t s)
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

static void set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// ── 跨站請求防護（CSRF）────────────────────────────────────────────────────────
//
// 這些端點沒有任何認證，而 /start、/stop 是不帶 body 的 POST —— 瀏覽器把這種
// 請求歸類為 simple request，不觸發 preflight。也就是說使用者只要開著某個惡意
// 網頁，那個網頁就能在背景對同網段的這台機器下指令，使用者完全不會察覺。
// /config 比「亂按開始充電」更嚴重：max_voltage 決定 0x508 的 VLIM2
// （車端的異常判定電壓上限），改壞它等於讓車輛的過壓保護失效。
//
// 解法：狀態變更端點一律要求一個自訂標頭。自訂標頭會強制瀏覽器先送 preflight
// (OPTIONS)，而本伺服器沒有註冊 OPTIONS handler，跨站請求就在那一步失敗。
// 同源請求（裝置自己供的那個網頁）根本不走 CORS，標頭直接送出，不受影響。
//
// ⚠️ 這不是認證。任何能直接發 HTTP 的東西（curl、腳本、同網段的程式）都能自己
// 加上這個標頭。它擋的是瀏覽器替使用者發起的跨站請求，不是有意的攻擊者。
#define CSRF_HEADER "X-TES-Request"

static bool csrf_ok(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, CSRF_HEADER) > 0) return true;
    set_cors(req);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                        "missing " CSRF_HEADER " header — cross-site request blocked");
    return false;
}

// 充電流程進行中（含 ENDING 收尾）→ true。
// OTA 會直接 esp_restart()，在這些狀態下重開機會讓繼電器/電磁鎖失去控制，
// 而且 PSU 仍保持最後的 setpoint 繼續輸出。
static bool charger_is_busy(void)
{
    tes_state_t st = TES_STATE_IDLE;
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        st = g_snapshot.state;
        xSemaphoreGive(g_snapshot_mutex);
    }
    return st == TES_STATE_PARAM_EXCHANGE || st == TES_STATE_PRE_CHARGE ||
           st == TES_STATE_CHARGING       || st == TES_STATE_ENDING;
}

// ── GET / (embedded web UI) ───────────────────────────────────────────────────

extern const uint8_t s_index_html_start[]   asm("_binary_index_html_start");
extern const uint8_t s_index_html_end[]     asm("_binary_index_html_end");
extern const uint8_t s_devices_html_start[] asm("_binary_devices_html_start");
extern const uint8_t s_devices_html_end[]   asm("_binary_devices_html_end");
extern const uint8_t s_manifest_json_start[] asm("_binary_manifest_json_start");
extern const uint8_t s_manifest_json_end[]   asm("_binary_manifest_json_end");
extern const uint8_t s_sw_js_start[]        asm("_binary_sw_js_start");
extern const uint8_t s_sw_js_end[]          asm("_binary_sw_js_end");
extern const uint8_t s_icon_svg_start[]     asm("_binary_icon_svg_start");
extern const uint8_t s_icon_svg_end[]       asm("_binary_icon_svg_end");

// "/"        → 裝置列表（先看到有哪幾台，再點進去）
// "/control" → 原本的控制介面
// 頁面內容綁在韌體裡，每次 OTA 都可能改變。max-age=86400 會讓使用者在更新後
// 最多卡在舊頁面 24 小時（且無從察覺）—— 對裝置自帶的 UI 是錯誤的取捨。
// 頁面只有數十 KB 又是區網直連，每次重新取用的成本遠低於顯示過期介面。
// 離線能力不受影響：Service Worker 的 Cache API 與 HTTP 快取是各自獨立的。
#define UI_CACHE_CONTROL "no-cache"

static esp_err_t handle_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", UI_CACHE_CONTROL);
    httpd_resp_send(req, (const char *)s_devices_html_start,
                    s_devices_html_end - s_devices_html_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_root = {
    .uri = "/", .method = HTTP_GET, .handler = handle_get_root
};

static esp_err_t handle_get_control(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", UI_CACHE_CONTROL);
    httpd_resp_send(req, (const char *)s_index_html_start,
                    s_index_html_end - s_index_html_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_control = {
    .uri = "/control", .method = HTTP_GET, .handler = handle_get_control
};

// ── GET /status ───────────────────────────────────────────────────────────────

static esp_err_t handle_get_status(httpd_req_t *req)
{
    tes_snapshot_t snap;
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = g_snapshot;
        xSemaphoreGive(g_snapshot_mutex);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot busy");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",             state_name(snap.state));
    cJSON_AddNumberToObject(root, "voltage",           snap.output_voltage);
    cJSON_AddNumberToObject(root, "current",           snap.output_current);
    cJSON_AddNumberToObject(root, "power_w",           snap.output_voltage * snap.output_current);
    cJSON_AddNumberToObject(root, "energy_wh",         snap.energy_wh);
    cJSON_AddBoolToObject  (root, "psu_connected",     snap.psu_connected);
    cJSON_AddNumberToObject(root, "soc",               snap.soc);
    cJSON_AddNumberToObject(root, "target_soc",        snap.target_soc);
    cJSON_AddBoolToObject  (root, "fault",             snap.fault_latched);
    cJSON_AddBoolToObject  (root, "charge_complete",   snap.charge_complete);
    cJSON_AddBoolToObject  (root, "timer_running",     snap.timer_running);
    cJSON_AddNumberToObject(root, "elapsed_seconds",   snap.elapsed_seconds);
    cJSON_AddNumberToObject(root, "remaining_seconds", snap.remaining_seconds);
    cJSON_AddNumberToObject(root, "fault_flags",       snap.last_fault_flags);
    cJSON_AddNumberToObject(root, "fault_source",      snap.fault_source);   // fault_source_t
    cJSON_AddNumberToObject(root, "fault_ctx_a",       snap.fault_ctx_a);    // 意義依 fault_source
    cJSON_AddNumberToObject(root, "fault_ctx_b",       snap.fault_ctx_b);
    cJSON_AddNumberToObject(root, "stop_reason",       snap.stop_reason);    // stop_reason_t
    cJSON_AddBoolToObject  (root, "wifi_connected",    s_connected);
    cJSON_AddStringToObject(root, "ip",                s_ip_str);

    // 裝置識別（多台同網段時用來分辨是哪一台）
    {
        const charger_config_t *dc = config_svc_get();
        char nice[40];
        friendly_name(dc, nice, sizeof(nice));
        cJSON_AddStringToObject(root, "device_id",    dc->device_id);
        cJSON_AddStringToObject(root, "device_name",  dc->device_name);
        cJSON_AddStringToObject(root, "display_name", nice);
        cJSON_AddStringToObject(root, "hostname",     s_hostname);
        cJSON_AddStringToObject(root, "ap_ssid",      s_ap_ssid);
    }

    // 充電停止條件（供 Web UI 顯示邏輯使用）
    const charger_config_t *cfg_snap = config_svc_get();
    cJSON_AddNumberToObject(root, "stop_mode",      (int)cfg_snap->stop_mode);
    cJSON_AddNumberToObject(root, "stop_voltage",   (double)cfg_snap->stop_voltage_01v / 10.0);
    cJSON_AddNumberToObject(root, "charge_timer_min", cfg_snap->charge_timer_min);

    // PSU 連線資訊（transport/pairing 來自 config；rssi/fail_streak 來自 driver 即時狀態）
    cJSON_AddNumberToObject(root, "psu_transport",  (int)cfg_snap->psu_transport);
    cJSON_AddBoolToObject  (root, "psu_has_peer",   cfg_snap->psu_paired);
    {
        psu_status_t psu_st = psu_driver_get_status();
        cJSON_AddNumberToObject(root, "psu_rssi",        (int)psu_st.rssi);
        cJSON_AddNumberToObject(root, "psu_fail_streak",  (int)psu_st.fail_streak);
    }

    // OTA 狀態
    ota_state_t ota_st = ota_svc_get_state();
    cJSON_AddBoolToObject  (root, "ota_running",  ota_st == OTA_STATE_RUNNING);
    cJSON_AddNumberToObject(root, "ota_progress", ota_svc_progress_pct());
    if (ota_st == OTA_STATE_ERROR)
        cJSON_AddStringToObject(root, "ota_error", ota_svc_get_error());

    // CAN 診斷
    cJSON *can = cJSON_AddObjectToObject(root, "can");
    // 0x500 Vehicle → Charger
    cJSON_AddNumberToObject(can, "v500_fault",        snap.can.v500_fault);
    cJSON_AddNumberToObject(can, "v500_status",       snap.can.v500_status);
    cJSON_AddNumberToObject(can, "v500_req_current",  snap.can.v500_req_current);
    cJSON_AddNumberToObject(can, "v500_req_voltage",  snap.can.v500_req_voltage);
    cJSON_AddNumberToObject(can, "v500_max_voltage",  snap.can.v500_max_voltage);
    // 0x501 Vehicle → Charger
    cJSON_AddNumberToObject(can, "v501_seq",          snap.can.v501_seq);
    cJSON_AddNumberToObject(can, "v501_soc",          snap.can.v501_soc);
    cJSON_AddNumberToObject(can, "v501_max_time",     snap.can.v501_max_time);
    cJSON_AddNumberToObject(can, "v501_eta",          snap.can.v501_eta);
    // 0x5F0 Vehicle → Charger
    cJSON_AddNumberToObject(can, "v5f0_flags",        snap.can.v5f0_flags);
    cJSON_AddNumberToObject(can, "v5f0_max_current",  snap.can.v5f0_max_current);
    cJSON_AddNumberToObject(can, "v5f0_maker",        snap.can.v5f0_maker);
    // 0x508 Charger → Vehicle
    cJSON_AddNumberToObject(can, "c508_fault",        snap.can.c508_fault);
    cJSON_AddNumberToObject(can, "c508_status",       snap.can.c508_status);
    cJSON_AddNumberToObject(can, "c508_avail_voltage",snap.can.c508_avail_voltage);
    cJSON_AddNumberToObject(can, "c508_avail_current",snap.can.c508_avail_current);
    cJSON_AddNumberToObject(can, "c508_fault_voltage",snap.can.c508_fault_voltage);
    // 0x509 Charger → Vehicle
    cJSON_AddNumberToObject(can, "c509_seq",          snap.can.c509_seq);
    cJSON_AddNumberToObject(can, "c509_rated_kw",     snap.can.c509_rated_kw);
    cJSON_AddNumberToObject(can, "c509_voltage",      snap.can.c509_voltage);
    cJSON_AddNumberToObject(can, "c509_current",      snap.can.c509_current);
    cJSON_AddNumberToObject(can, "c509_remaining",    snap.can.c509_remaining);
    // 0x5F8 Charger → Vehicle
    cJSON_AddNumberToObject(can, "c5f8_flags",        snap.can.c5f8_flags);
    cJSON_AddNumberToObject(can, "c5f8_maker",        snap.can.c5f8_maker);
    // 收發活性（age = 距離最後一次收到的毫秒；4294967295 表示從未收到）
    cJSON_AddNumberToObject(can, "rx_500_count",  snap.can.rx_500_count);
    cJSON_AddNumberToObject(can, "rx_501_count",  snap.can.rx_501_count);
    cJSON_AddNumberToObject(can, "rx_5f0_count",  snap.can.rx_5f0_count);
    cJSON_AddNumberToObject(can, "rx_500_age_ms", snap.can.rx_500_age_ms);
    cJSON_AddNumberToObject(can, "rx_501_age_ms", snap.can.rx_501_age_ms);
    cJSON_AddNumberToObject(can, "rx_5f0_age_ms", snap.can.rx_5f0_age_ms);
    cJSON_AddNumberToObject(can, "tx_508_count",  snap.can.tx_508_count);
    cJSON_AddNumberToObject(can, "tx_509_count",  snap.can.tx_509_count);
    cJSON_AddNumberToObject(can, "tx_5f8_count",  snap.can.tx_5f8_count);
    cJSON_AddNumberToObject(can, "tx_fail_count", snap.can.tx_fail_count);
    // TWAI 控制器健康度
    cJSON_AddNumberToObject(can, "bus_state",     snap.can.bus_state);
    cJSON_AddNumberToObject(can, "bus_tx_err",    snap.can.bus_tx_err);
    cJSON_AddNumberToObject(can, "bus_rx_err",    snap.can.bus_rx_err);
    cJSON_AddNumberToObject(can, "bus_arb_lost",  snap.can.bus_arb_lost);
    cJSON_AddNumberToObject(can, "bus_err_count", snap.can.bus_err_count);
    cJSON_AddNumberToObject(can, "bus_rx_missed", snap.can.bus_rx_missed);

    // NTP / 定時充電時間
    bool ntp_synced;
    char local_time[20];
    scheduler_svc_get_time_info(&ntp_synced, local_time, sizeof(local_time));
    cJSON_AddBoolToObject  (root, "ntp_synced",  ntp_synced);
    cJSON_AddStringToObject(root, "local_time",  local_time);

    // 韌體版本
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "firmware_version", app->version);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_status = {
    .uri = "/status", .method = HTTP_GET, .handler = handle_get_status
};

// ── GET /config ───────────────────────────────────────────────────────────────

static esp_err_t handle_get_config(httpd_req_t *req)
{
    const charger_config_t *cfg = config_svc_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id",      cfg->device_id);
    cJSON_AddStringToObject(root, "device_name",    cfg->device_name);
    cJSON_AddStringToObject(root, "hostname",       s_hostname);
    cJSON_AddStringToObject(root, "ap_ssid",        s_ap_ssid);
    cJSON_AddBoolToObject  (root, "auto_voltage",   cfg->auto_voltage);
    cJSON_AddNumberToObject(root, "max_voltage",    (double)cfg->max_voltage_01v / 10.0);
    cJSON_AddNumberToObject(root, "max_current",    (double)cfg->max_current_01a / 10.0);
    cJSON_AddNumberToObject(root, "target_soc",     cfg->target_soc);
    cJSON_AddNumberToObject(root, "stop_mode",        (int)cfg->stop_mode);
    cJSON_AddNumberToObject(root, "stop_voltage",     (double)cfg->stop_voltage_01v / 10.0);
    cJSON_AddNumberToObject(root, "charge_timer_min", cfg->charge_timer_min);
    cJSON_AddStringToObject(root, "wifi_ssid",      cfg->wifi_ssid);
    cJSON_AddBoolToObject  (root, "sta_enabled",    cfg->sta_enabled);
    cJSON_AddBoolToObject  (root, "beacon",         cfg->beacon_unlocked);
    cJSON_AddStringToObject(root, "notify_url",        cfg->notify_url);
    cJSON_AddStringToObject(root, "mqtt_broker_url",   cfg->mqtt_broker_url);
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
    cJSON_AddBoolToObject  (root, "sched_enabled",   cfg->sched_enabled);
    cJSON_AddNumberToObject(root, "sched_start_min", cfg->sched_start_min);
    cJSON_AddBoolToObject  (root, "sched_stop_en",   cfg->sched_stop_en);
    cJSON_AddNumberToObject(root, "sched_stop_min",  cfg->sched_stop_min);
    cJSON_AddBoolToObject  (root, "auto_start",      cfg->auto_start);
    cJSON_AddNumberToObject(root, "psu_transport",   cfg->psu_transport);
    cJSON_AddBoolToObject  (root, "psu_paired",      cfg->psu_paired);
    char mac_str[18] = "";
    if (cfg->psu_paired) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg->psu_peer_mac[0], cfg->psu_peer_mac[1], cfg->psu_peer_mac[2],
                 cfg->psu_peer_mac[3], cfg->psu_peer_mac[4], cfg->psu_peer_mac[5]);
    }
    cJSON_AddStringToObject(root, "psu_peer_mac",    mac_str);
    cJSON_AddBoolToObject  (root, "psu_pairing",     psu_driver_is_pairing());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_config = {
    .uri = "/config", .method = HTTP_GET, .handler = handle_get_config
};

// ── POST /config ──────────────────────────────────────────────────────────────
// Partial update: only supplied fields are written to NVS.
// Accepted fields (all optional):
//   max_voltage  float V   (40.0–120.0)
//   max_current  float A   (1.0–100.0)
//   target_soc   int %     (20–100)
//   wifi_ssid    string
//   wifi_pass    string
//   beacon       bool

static esp_err_t handle_post_config(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size invalid");
        return ESP_FAIL;
    }

    char body[MAX_BODY + 1];
    int len = httpd_req_recv(req, body, req->content_len);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    const charger_config_t *cur = config_svc_get();
    uint16_t new_voltage = cur->max_voltage_01v;
    uint16_t new_current = cur->max_current_01a;
    int8_t   new_soc     = cur->target_soc;
    char     new_ssid[33];
    char     new_pass[64];
    strncpy(new_ssid, cur->wifi_ssid, sizeof(new_ssid) - 1);
    new_ssid[sizeof(new_ssid) - 1] = '\0';
    strncpy(new_pass, cur->wifi_pass, sizeof(new_pass) - 1);
    new_pass[sizeof(new_pass) - 1] = '\0';
    bool new_beacon         = cur->beacon_unlocked;
    bool new_auto_voltage   = cur->auto_voltage;
    uint8_t  new_stop_mode     = (uint8_t)cur->stop_mode;
    uint16_t new_stop_voltage  = cur->stop_voltage_01v;
    uint16_t new_charge_timer  = cur->charge_timer_min;

    bool charging_changed     = false;
    bool wifi_changed         = false;
    bool beacon_changed       = false;
    bool auto_voltage_changed = false;
    bool stop_changed         = false;

    cJSON *item;

    item = cJSON_GetObjectItem(root, "max_voltage");
    if (cJSON_IsNumber(item)) {
        int v = (int)(item->valuedouble * 10.0 + 0.5);
        if (v >= 400 && v <= 1200) { new_voltage = (uint16_t)v; charging_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "max_current");
    if (cJSON_IsNumber(item)) {
        int a = (int)(item->valuedouble * 10.0 + 0.5);
        if (a >= 10 && a <= 1000) { new_current = (uint16_t)a; charging_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "target_soc");
    if (cJSON_IsNumber(item)) {
        int s = item->valueint;
        if (s >= 20 && s <= 100) { new_soc = (int8_t)s; charging_changed = true; }
    }
    char new_device_name[25] = {0};
    bool device_name_changed = false;
    item = cJSON_GetObjectItem(root, "device_name");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_device_name, item->valuestring, sizeof(new_device_name) - 1);
        device_name_changed = true;
    }

    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_ssid, item->valuestring, sizeof(new_ssid) - 1);
        new_ssid[sizeof(new_ssid) - 1] = '\0';
        wifi_changed = true;
    }
    item = cJSON_GetObjectItem(root, "wifi_pass");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_pass, item->valuestring, sizeof(new_pass) - 1);
        new_pass[sizeof(new_pass) - 1] = '\0';
        wifi_changed = true;
    }
    // STA 開關：切換等同 WiFi 模式變更，要走同一條「需重啟」的回報路徑
    item = cJSON_GetObjectItem(root, "sta_enabled");
    if (cJSON_IsBool(item)) {
        bool en = cJSON_IsTrue(item);
        if (en != cur->sta_enabled) {
            config_svc_set_sta_enabled(en);
            wifi_changed = true;
        }
    }
    item = cJSON_GetObjectItem(root, "beacon");
    if (cJSON_IsBool(item)) {
        new_beacon = cJSON_IsTrue(item);
        beacon_changed = true;
    }
    item = cJSON_GetObjectItem(root, "auto_voltage");
    if (cJSON_IsBool(item)) {
        new_auto_voltage = cJSON_IsTrue(item);
        auto_voltage_changed = true;
    }
    item = cJSON_GetObjectItem(root, "stop_mode");
    if (cJSON_IsNumber(item)) {
        int m = item->valueint;
        if (m >= 0 && m <= 2) { new_stop_mode = (uint8_t)m; stop_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "stop_voltage");
    if (cJSON_IsNumber(item)) {
        int v = (int)(item->valuedouble * 10.0 + 0.5);
        if (v >= 400 && v <= 1200) { new_stop_voltage = (uint16_t)v; stop_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "charge_timer_min");
    if (cJSON_IsNumber(item)) {
        int t = item->valueint;
        if (t >= 1 && t <= 600) { new_charge_timer = (uint16_t)t; stop_changed = true; }
    }

    char new_notify_url[128] = {0};
    bool notify_url_changed = false;
    item = cJSON_GetObjectItem(root, "notify_url");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_notify_url, item->valuestring, sizeof(new_notify_url) - 1);
        notify_url_changed = true;
    }

    bool     new_sched_enabled   = cur->sched_enabled;
    uint16_t new_sched_start_min = cur->sched_start_min;
    bool     new_sched_stop_en   = cur->sched_stop_en;
    uint16_t new_sched_stop_min  = cur->sched_stop_min;
    bool     sched_changed       = false;

    item = cJSON_GetObjectItem(root, "sched_enabled");
    if (cJSON_IsBool(item)) { new_sched_enabled = cJSON_IsTrue(item); sched_changed = true; }
    item = cJSON_GetObjectItem(root, "sched_start_min");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 0 && v < 1440) { new_sched_start_min = (uint16_t)v; sched_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "sched_stop_en");
    if (cJSON_IsBool(item)) { new_sched_stop_en = cJSON_IsTrue(item); sched_changed = true; }
    item = cJSON_GetObjectItem(root, "sched_stop_min");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 0 && v < 1440) { new_sched_stop_min = (uint16_t)v; sched_changed = true; }
    }

    char new_mqtt_broker_url[128]   = {0};
    char new_mqtt_topic_prefix[64]  = {0};
    bool mqtt_changed = false;
    item = cJSON_GetObjectItem(root, "mqtt_broker_url");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_mqtt_broker_url, item->valuestring, sizeof(new_mqtt_broker_url) - 1);
        mqtt_changed = true;
    }
    item = cJSON_GetObjectItem(root, "mqtt_topic_prefix");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_mqtt_topic_prefix, item->valuestring, sizeof(new_mqtt_topic_prefix) - 1);
        mqtt_changed = true;
    }
    // Fill in current values if only one field was supplied
    if (mqtt_changed) {
        if (new_mqtt_broker_url[0] == '\0')
            strncpy(new_mqtt_broker_url, config_svc_get()->mqtt_broker_url, sizeof(new_mqtt_broker_url) - 1);
        if (new_mqtt_topic_prefix[0] == '\0')
            strncpy(new_mqtt_topic_prefix, config_svc_get()->mqtt_topic_prefix, sizeof(new_mqtt_topic_prefix) - 1);
    }

    bool new_auto_start    = cur->auto_start;
    bool auto_start_changed = false;
    item = cJSON_GetObjectItem(root, "auto_start");
    if (cJSON_IsBool(item)) { new_auto_start = cJSON_IsTrue(item); auto_start_changed = true; }

    uint8_t new_psu_transport = cur->psu_transport;
    bool    psu_transport_changed = false;
    item = cJSON_GetObjectItem(root, "psu_transport");
    if (cJSON_IsNumber(item)) {
        int t = item->valueint;
        if (t == 0 || t == 1) { new_psu_transport = (uint8_t)t; psu_transport_changed = true; }
    }

    cJSON_Delete(root);

    if (device_name_changed) {
        config_svc_set_device_name(new_device_name);
        // mDNS 的 instance name 與 TXT 是廣告出去的內容，改名要立刻反映，
        // 否則服務瀏覽器上還是舊名字。主機名 tes-<id> 不受影響（刻意保持穩定）。
        if (s_mdns_ok) {
            char nice[40];
            friendly_name(config_svc_get(), nice, sizeof(nice));
            mdns_instance_name_set(nice);
            mdns_service_txt_item_set("_http", "_tcp", "name", nice);
            ESP_LOGI(TAG, "device renamed to \"%s\"", nice);
        }
    }
    if (charging_changed)     config_svc_set_charging(new_voltage, new_current, new_soc);
    if (wifi_changed)         config_svc_set_wifi(new_ssid, new_pass);
    if (beacon_changed)       config_svc_set_beacon(new_beacon);
    if (auto_voltage_changed) config_svc_set_auto_voltage(new_auto_voltage);
    if (notify_url_changed)   config_svc_set_notify_url(new_notify_url);
    if (stop_changed)         config_svc_set_stop((stop_mode_t)new_stop_mode, new_stop_voltage, new_charge_timer);
    if (mqtt_changed)         config_svc_set_mqtt(new_mqtt_broker_url, new_mqtt_topic_prefix);
    if (sched_changed)        config_svc_set_scheduler(new_sched_enabled, new_sched_start_min, new_sched_stop_en, new_sched_stop_min);
    if (auto_start_changed)   config_svc_set_auto_start(new_auto_start);
    if (psu_transport_changed)
        config_svc_set_psu(new_psu_transport, cur->psu_peer_mac, cur->psu_paired);

    // 這三項都只在開機時被讀取一次（WiFi 在 network_svc_init、MQTT 在 task_mqtt
    // 啟動時、PSU transport 在 app_main 末端），改完必須重開機才會生效。
    bool reboot_required = wifi_changed || mqtt_changed || psu_transport_changed;
    if (reboot_required)
        ESP_LOGI(TAG, "WiFi/MQTT/PSU-transport config updated — reboot to apply");

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, reboot_required
        ? "{\"ok\":true,\"reboot_required\":true,"
          "\"note\":\"reboot to apply wifi / mqtt / psu_transport changes\"}"
        : "{\"ok\":true,\"reboot_required\":false}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_config = {
    .uri = "/config", .method = HTTP_POST, .handler = handle_post_config
};

// ── POST /start ───────────────────────────────────────────────────────────────

static esp_err_t handle_post_start(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    uint8_t evt = EVT_BUTTON_START;
    xQueueSendToBack(g_btn_event_queue, &evt, 0);
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_start = {
    .uri = "/start", .method = HTTP_POST, .handler = handle_post_start
};

// ── POST /stop ────────────────────────────────────────────────────────────────

static esp_err_t handle_post_stop(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    uint8_t evt = EVT_BUTTON_STOP;
    xQueueSendToBack(g_btn_event_queue, &evt, 0);
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_stop = {
    .uri = "/stop", .method = HTTP_POST, .handler = handle_post_stop
};

// ── POST /ota ─────────────────────────────────────────────────────────────────
// Body (optional): {"url": "https://..."}  — 省略則使用預設 GitHub Releases URL

static esp_err_t handle_post_ota(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    char url[256] = "";

    if (charger_is_busy()) {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "charging in progress — stop the session before updating");
        return ESP_FAIL;
    }

    if (req->content_len > 0 && req->content_len <= MAX_BODY) {
        char body[MAX_BODY + 1];
        int len = httpd_req_recv(req, body, req->content_len);
        if (len > 0) {
            body[len] = '\0';
            cJSON *root = cJSON_Parse(body);
            if (root) {
                cJSON *item = cJSON_GetObjectItem(root, "url");
                if (cJSON_IsString(item) && item->valuestring)
                    strncpy(url, item->valuestring, sizeof(url) - 1);
                cJSON_Delete(root);
            }
        }
    }

    esp_err_t err = ota_svc_start(url);
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA start failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_ota = {
    .uri = "/ota", .method = HTTP_POST, .handler = handle_post_ota
};

// ── POST /ota/upload ──────────────────────────────────────────────────────────
// Body: raw firmware binary (application/octet-stream), Content-Length required

#define OTA_UPLOAD_BUF 4096

static esp_err_t handle_post_ota_upload(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    if (charger_is_busy()) {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "charging in progress — stop the session before updating");
        return ESP_FAIL;
    }
    if (ota_svc_get_state() == OTA_STATE_RUNNING) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    esp_err_t err = ota_svc_upload_begin((size_t)req->content_len);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    static char buf[OTA_UPLOAD_BUF];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int to_recv = remaining < OTA_UPLOAD_BUF ? remaining : OTA_UPLOAD_BUF;
        int received = httpd_req_recv(req, buf, (size_t)to_recv);
        if (received <= 0) {
            ota_svc_upload_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        err = ota_svc_upload_write(buf, (size_t)received);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ota_svc_upload_end(); // validates, sets boot partition, reboots
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_ota_upload = {
    .uri = "/ota/upload", .method = HTTP_POST, .handler = handle_post_ota_upload
};

// ── GET /manifest.json ────────────────────────────────────────────────────────

static esp_err_t handle_get_manifest(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/manifest+json");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, (const char *)s_manifest_json_start,
                    s_manifest_json_end - s_manifest_json_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_manifest = {
    .uri = "/manifest.json", .method = HTTP_GET, .handler = handle_get_manifest
};

// ── GET /sw.js ────────────────────────────────────────────────────────────────

static esp_err_t handle_get_sw(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");   // SW must always be fresh
    httpd_resp_send(req, (const char *)s_sw_js_start,
                    s_sw_js_end - s_sw_js_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_sw = {
    .uri = "/sw.js", .method = HTTP_GET, .handler = handle_get_sw
};

// ── GET /icon.svg ─────────────────────────────────────────────────────────────

static esp_err_t handle_get_icon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_send(req, (const char *)s_icon_svg_start,
                    s_icon_svg_end - s_icon_svg_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_icon = {
    .uri = "/icon.svg", .method = HTTP_GET, .handler = handle_get_icon
};

// ── GET /wifi/scan ────────────────────────────────────────────────────────────
// Performs an active scan and returns up to 20 APs sorted by RSSI.
// Works in both AP and STA mode (device initialises as APSTA so scanning is
// always available regardless of whether an SSID has been configured).

static esp_err_t handle_get_wifi_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .scan_type              = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min   = 100,
        .scan_time.active.max   = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   // blocking ~300 ms
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    cJSON *arr = cJSON_CreateArray();
    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (!ap_list) {
            cJSON_Delete(arr);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
            return ESP_FAIL;
        }
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        for (int i = 0; i < ap_count; i++) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "ssid",    (const char *)ap_list[i].ssid);
            cJSON_AddNumberToObject(obj, "rssi",    ap_list[i].rssi);
            cJSON_AddBoolToObject  (obj, "secured", ap_list[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, obj);
        }
        free(ap_list);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_wifi_scan = {
    .uri = "/wifi/scan", .method = HTTP_GET, .handler = handle_get_wifi_scan
};

// ── GET /history ──────────────────────────────────────────────────────────────

static esp_err_t handle_get_history(httpd_req_t *req)
{
    charge_session_t buf[20];
    uint8_t n = log_svc_get_history(buf, 20);

    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < n; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "duration_s",       buf[i].duration_s);
        cJSON_AddNumberToObject(obj, "energy_wh",        (double)buf[i].energy_wh);
        cJSON_AddBoolToObject  (obj, "energy_estimated", buf[i].energy_estimated);
        cJSON_AddNumberToObject(obj, "stop_voltage_v",   (double)buf[i].stop_voltage_v);
        cJSON_AddNumberToObject(obj, "soc_start",        buf[i].soc_start);
        cJSON_AddNumberToObject(obj, "soc_end",          buf[i].soc_end);
        cJSON_AddNumberToObject(obj, "stop_reason",      buf[i].stop_reason);
        cJSON_AddNumberToObject(obj, "session_id",       buf[i].session_id);
        cJSON_AddNumberToObject(obj, "fault_source",     buf[i].fault_source);
        cJSON_AddNumberToObject(obj, "fault_ctx_a",      buf[i].fault_ctx_a);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_history = {
    .uri = "/history", .method = HTTP_GET, .handler = handle_get_history
};

// ── GET /devices ──────────────────────────────────────────────────────────────
// 用 mDNS PTR 查詢列出區網上的 TES 控制器（含自己）。
// 只有 TXT 帶 dev=tes-charger 的才算，用來濾掉 NAS / 印表機之類的 _http._tcp 服務。
//
// 這件事必須在裝置端做：瀏覽器沒有 mDNS 瀏覽 API，逐一掃 IP 又慢又容易被擋。

static const char *txt_get(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0)
            return r->txt[i].value ? r->txt[i].value : "";
    }
    return NULL;
}

static esp_err_t handle_get_devices(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    const charger_config_t *cfg = config_svc_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "self_id", cfg->device_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");

    bool self_seen = false;
    mdns_result_t *results = NULL;
    // 2 秒足夠涵蓋一般家用網段；HTTP handler 阻塞這段時間可以接受
    if (s_mdns_ok && mdns_query_ptr("_http", "_tcp", 2000, 20, &results) == ESP_OK) {
        for (mdns_result_t *r = results; r; r = r->next) {
            const char *mark = txt_get(r, "dev");
            if (!mark || strcmp(mark, MDNS_DEV_MARKER) != 0) continue;   // 不是我們的裝置

            const char *id = txt_get(r, "id");
            if (id && strcmp(id, cfg->device_id) == 0) self_seen = true;

            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id",       id ? id : "");
            cJSON_AddStringToObject(o, "name",     txt_get(r, "name") ? txt_get(r, "name") : "");
            cJSON_AddStringToObject(o, "version",  txt_get(r, "ver")  ? txt_get(r, "ver")  : "");
            cJSON_AddStringToObject(o, "hostname", r->hostname ? r->hostname : "");
            cJSON_AddNumberToObject(o, "port",     r->port ? r->port : 80);
            cJSON_AddBoolToObject  (o, "self",     id && strcmp(id, cfg->device_id) == 0);

            char ipbuf[16] = "";
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                    break;
                }
            }
            cJSON_AddStringToObject(o, "ip", ipbuf);
            cJSON_AddItemToArray(arr, o);
        }
        mdns_query_results_free(results);
    }

    // 自己不一定會出現在查詢結果裡（多數 mDNS 實作不回應自己發出的查詢），
    // 沒看到就補上，否則列表會少一台。
    if (!self_seen) {
        char nice[40];
        friendly_name(cfg, nice, sizeof(nice));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",       cfg->device_id);
        cJSON_AddStringToObject(o, "name",     nice);
        cJSON_AddStringToObject(o, "version",  esp_app_get_description()->version);
        cJSON_AddStringToObject(o, "hostname", s_hostname);
        cJSON_AddNumberToObject(o, "port",     80);
        cJSON_AddBoolToObject  (o, "self",     true);
        cJSON_AddStringToObject(o, "ip",       s_ip_str);
        cJSON_AddItemToArray(arr, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { httpd_resp_sendstr(req, json); free(json); }
    else      { httpd_resp_sendstr(req, "{\"devices\":[]}"); }
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_devices = {
    .uri = "/devices", .method = HTTP_GET, .handler = handle_get_devices
};

// ── GET /trace ────────────────────────────────────────────────────────────────
// 充電曲線取樣。?session=<id> 省略時取最新一筆 session。
// 回應同時附上 sessions 清單，前端可直接用來做下拉選單。
// samples 用緊湊陣列格式 [t_ms, V*10, I*10, BMSreq*10, soc, state] 以縮小體積。

static uint32_t query_u32(httpd_req_t *req, const char *key, uint32_t defval)
{
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return defval;
    char v[24];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return defval;
    return (uint32_t)strtoul(v, NULL, 10);
}

// ── 分塊輸出緩衝 ──────────────────────────────────────────────────────────────
// httpd_resp_sendstr_chunk() 每呼叫一次就送出一個 TCP 分段。曲線與 Log 若每筆
// 各送一次，數百筆就是數百次往返 —— 在 WiFi 延遲下慢到讓前端以為裝置離線，
// 而且期間會一直佔住 httpd 唯一的工作執行緒，讓 /status 等其他請求全部排隊。
// 累積到接近 1KB 才送，往返次數降低約兩個數量級。
typedef struct {
    httpd_req_t *req;
    size_t       len;
    char         buf[1024];
} chunk_out_t;

static void chunk_init(chunk_out_t *co, httpd_req_t *req)
{
    co->req = req;
    co->len = 0;
}

static void chunk_flush(chunk_out_t *co)
{
    if (co->len) {
        httpd_resp_send_chunk(co->req, co->buf, co->len);
        co->len = 0;
    }
}

static void chunk_puts(chunk_out_t *co, const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof(co->buf)) {          // 單筆就超過緩衝區：先清空再直送
        chunk_flush(co);
        httpd_resp_send_chunk(co->req, s, n);
        return;
    }
    if (co->len + n > sizeof(co->buf)) chunk_flush(co);
    memcpy(co->buf + co->len, s, n);
    co->len += n;
}

static void trace_emit_sessions(chunk_out_t *co)
{
    trace_session_info_t list[TRACE_MAX_SESSIONS];
    int n = trace_svc_get_sessions(list, TRACE_MAX_SESSIONS);
    char buf[160];

    chunk_puts(co, "\"sessions\":[");
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":%lu,\"start_epoch\":%lu,\"start_uptime_ms\":%lu,"
                 "\"samples\":%lu,\"events\":%lu}",
                 i ? "," : "",
                 (unsigned long)list[i].session_id,
                 (unsigned long)list[i].start_epoch_s,
                 (unsigned long)list[i].start_t_ms,
                 (unsigned long)list[i].sample_count,
                 (unsigned long)list[i].event_count);
        chunk_puts(co, buf);
    }
    chunk_puts(co, "]");
}

static esp_err_t handle_get_trace(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    if (!trace_svc_is_ready()) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"trace buffer unavailable\"}");
        return ESP_OK;
    }

    uint32_t sid = query_u32(req, "session", 0);
    if (sid == 0) sid = trace_svc_newest_session();

    uint32_t total = trace_svc_sample_count(sid);
    uint32_t limit = query_u32(req, "limit", 2000);
    if (limit == 0 || limit > 8192) limit = 8192;

    // 超過上限時等間隔抽樣，維持曲線形狀
    uint32_t step = (total > limit) ? ((total + limit - 1) / limit) : 1;

    static chunk_out_t co;   // 1KB，放 static 避免佔用 httpd 的 8KB 堆疊
    chunk_init(&co, req);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"session\":%lu,\"total\":%lu,\"step\":%lu,",
             (unsigned long)sid, (unsigned long)total, (unsigned long)step);
    chunk_puts(&co, buf);
    trace_emit_sessions(&co);

    chunk_puts(&co, ",\"samples\":[");
    bool first = true;
    for (uint32_t i = 0; i < total; i += step) {
        trace_sample_t s;
        if (!trace_svc_get_sample(sid, i, &s)) continue;   // 已被環形覆蓋則跳過
        snprintf(buf, sizeof(buf), "%s[%lu,%u,%u,%u,%u,%u]",
                 first ? "" : ",",
                 (unsigned long)s.t_ms,
                 (unsigned)s.voltage_01v, (unsigned)s.current_01a,
                 (unsigned)s.req_current_01a, (unsigned)s.soc, (unsigned)s.state);
        chunk_puts(&co, buf);
        first = false;
    }
    chunk_puts(&co, "]}");
    chunk_flush(&co);
    httpd_resp_sendstr_chunk(req, NULL);   // 結束 chunked 回應
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_trace = {
    .uri = "/trace", .method = HTTP_GET, .handler = handle_get_trace
};

// ── GET /tracelog ─────────────────────────────────────────────────────────────
// 數值變動事件 Log。?session=<id>&limit=N（預設回傳最新的 N 筆）

static esp_err_t handle_get_tracelog(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    if (!trace_svc_is_ready()) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"trace buffer unavailable\"}");
        return ESP_OK;
    }

    uint32_t sid = query_u32(req, "session", 0);
    if (sid == 0) sid = trace_svc_newest_session();

    uint32_t total = trace_svc_event_count(sid);
    uint32_t limit = query_u32(req, "limit", 1000);
    if (limit == 0 || limit > 6144) limit = 6144;

    uint32_t start = (total > limit) ? (total - limit) : 0;   // 取最新的 limit 筆

    static chunk_out_t co;   // 1KB，放 static 避免佔用 httpd 的 8KB 堆疊
    chunk_init(&co, req);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"session\":%lu,\"total\":%lu,\"from\":%lu,\"events\":[",
             (unsigned long)sid, (unsigned long)total, (unsigned long)start);
    chunk_puts(&co, buf);

    bool first = true;
    for (uint32_t i = start; i < total; i++) {
        trace_event_t e;
        if (!trace_svc_get_event(sid, i, &e)) continue;

        // JSON 字串轉義：文字由韌體自行產生，只可能出現 " 與 \，處理這兩個即可
        char esc[TRACE_LOG_TEXT_LEN * 2];
        size_t k = 0;
        for (size_t j = 0; e.text[j] && k < sizeof(esc) - 2; j++) {
            if (e.text[j] == '"' || e.text[j] == '\\') esc[k++] = '\\';
            esc[k++] = e.text[j];
        }
        esc[k] = '\0';

        snprintf(buf, sizeof(buf), "%s[%lu,%lu,\"%s\"]",
                 first ? "" : ",",
                 (unsigned long)e.t_ms, (unsigned long)e.epoch_s, esc);
        chunk_puts(&co, buf);
        first = false;
    }
    chunk_puts(&co, "]}");
    chunk_flush(&co);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_tracelog = {
    .uri = "/tracelog", .method = HTTP_GET, .handler = handle_get_tracelog
};

// ── POST /notify/test ─────────────────────────────────────────────────────────

static esp_err_t handle_post_notify_test(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    const char *url = config_svc_get()->notify_url;
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (url[0] == '\0') {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"notify_url not configured\"}");
        return ESP_OK;
    }
    if (!s_connected) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no internet (AP mode or disconnected)\"}");
        return ESP_OK;
    }
    esp_err_t err = notify_svc_send(url, "TES 充電控制器", "推播通知測試成功 ✓", 3);
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send failed\"}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_notify_test = {
    .uri = "/notify/test", .method = HTTP_POST, .handler = handle_post_notify_test
};

// ── POST /psu/pair ────────────────────────────────────────────────────────────

static esp_err_t handle_post_psu_pair(httpd_req_t *req)
{
    if (!csrf_ok(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    if (config_svc_get()->psu_transport != PSU_TRANSPORT_ESPNOW) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"psu_transport is not ESP-NOW\"}");
        return ESP_OK;
    }
    psu_driver_start_pairing(NULL);   // callback 在 psu_driver_set_transport() 時已由 main.c 登錄
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"pairing window open (10s)\"}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_psu_pair = {
    .uri = "/psu/pair", .method = HTTP_POST, .handler = handle_post_psu_pair
};

// ── GET /mqtt/link ────────────────────────────────────────────────────────────
// Returns the Cloud PWA URL pre-filled with broker hostname, WS port, and topic.
// WS port mapping: HiveMQ→8000, Mosquitto→8080, others→8083 (EMQX default).

static void extract_broker_host(const char *uri, char *host, size_t len)
{
    // Strip scheme: "mqtt://user:pass@host:port" → "host"
    const char *p = strstr(uri, "://");
    p = p ? p + 3 : uri;
    // Skip user:pass@ if present
    const char *at = strchr(p, '@');
    if (at && (!strchr(p, ':') || strchr(p, ':') > at)) p = at + 1;
    const char *end = strchr(p, ':');
    if (!end) end = strchr(p, '/');
    if (!end) end = p + strlen(p);
    size_t n = (size_t)(end - p) < len - 1 ? (size_t)(end - p) : len - 1;
    memcpy(host, p, n);
    host[n] = '\0';
}

static uint16_t broker_ws_port(const char *host)
{
    if (strstr(host, "hivemq.com"))    return 8000;
    if (strstr(host, "mosquitto.org")) return 8080;
    return 8083;
}

static esp_err_t handle_get_mqtt_link(httpd_req_t *req)
{
    const charger_config_t *cfg = config_svc_get();
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    if (cfg->mqtt_broker_url[0] == '\0') {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"mqtt_broker_url not configured\"}");
        return ESP_OK;
    }

    char host[128];
    extract_broker_host(cfg->mqtt_broker_url, host, sizeof(host));
    uint16_t ws_port = broker_ws_port(host);

    // URL-encode topic prefix (encode '/' as %2F for query param safety)
    char topic_enc[128] = {0};
    const char *src = cfg->mqtt_topic_prefix;
    size_t ti = 0;
    while (*src && ti < sizeof(topic_enc) - 4) {
        if (*src == '/') {
            topic_enc[ti++] = '%';
            topic_enc[ti++] = '2';
            topic_enc[ti++] = 'F';
        } else {
            topic_enc[ti++] = *src;
        }
        src++;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "https://a950523a.github.io/TES-Taiwan-Electric-Scooter-Charging-Controller"
             "/monitor.html#b=%s&p=%u&t=%s",
             host, (unsigned)ws_port, topic_enc);

    char resp[420];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"url\":\"%s\"}", url);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_mqtt_link = {
    .uri = "/mqtt/link", .method = HTTP_GET, .handler = handle_get_mqtt_link
};

// ── WiFi event handler ────────────────────────────────────────────────────────

// 建立（或更新）mDNS 廣告。AP 與 STA 兩條路徑共用。
// ip4 = 目前這個介面的 IPv4，delegated hostname 必須明確給位址。
static void mdns_publish(uint32_t ip4_addr)
{
    const charger_config_t *cfg = config_svc_get();
    char nice[40];
    friendly_name(cfg, nice, sizeof(nice));

    if (!s_mdns_ok) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init failed");
            return;
        }
        mdns_hostname_set(s_hostname);
        mdns_instance_name_set(nice);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        // 讓區網掃描能直接辨識是哪一台，不必逐一開網頁確認。
        // "dev" 是我們自己的標記，GET /devices 用它把本產品和區網上其他
        // _http._tcp 服務（NAS、印表機…）區分開。
        mdns_service_txt_item_set("_http", "_tcp", "dev",  MDNS_DEV_MARKER);
        mdns_service_txt_item_set("_http", "_tcp", "id",   cfg->device_id);
        mdns_service_txt_item_set("_http", "_tcp", "name", nice);
        mdns_service_txt_item_set("_http", "_tcp", "ver",
                                  esp_app_get_description()->version);
        s_mdns_ok = true;
        ESP_LOGI(TAG, "mDNS: %s.local (\"%s\")", s_hostname, nice);
    }

    // 相容別名 tes-charger.local —— 兩台都掛時由其中一台回應，
    // 但各自的 tes-<id>.local 一定準確。失敗不影響主要主機名。
    mdns_ip_addr_t a = {0};
    a.addr.type            = ESP_IPADDR_TYPE_V4;
    a.addr.u_addr.ip4.addr = ip4_addr;
    a.next                 = NULL;
    esp_err_t dr = mdns_delegate_hostname_add(LEGACY_HOSTNAME, &a);
    if (dr != ESP_OK) dr = mdns_delegate_hostname_set_address(LEGACY_HOSTNAME, &a);

    // 光註冊 delegated hostname 不一定足夠 —— 掛一個服務在它底下，
    // 確保 mDNS 會為這個名字回應 A 查詢（否則 tes-charger.local 可能查不到）。
    if (dr == ESP_OK && !s_legacy_svc_ok) {
        if (mdns_service_add_for_host("TES Charger", "_http", "_tcp",
                                      LEGACY_HOSTNAME, 80, NULL, 0) == ESP_OK) {
            s_legacy_svc_ok = true;
            ESP_LOGI(TAG, "mDNS alias: %s.local -> %s", LEGACY_HOSTNAME, s_ip_str);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_START) {
            // AP mode is up — our IP is always 192.168.4.1
            strncpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str) - 1);
            ESP_LOGI(TAG, "AP \"%s\" started @ %s", s_ap_ssid, s_ip_str);
            mdns_publish(ESP_IP4TOADDR(192, 168, 4, 1));
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected  = false;
            s_ip_str[0]  = '\0';
            if (!s_ap_mode) esp_wifi_connect();  // auto-reconnect (not in AP-only setup mode)
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_connected = true;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));

        // DHCP 換過 IP 也會再進來一次，delegated hostname 的位址要跟著更新
        mdns_publish(ev->ip_info.ip.addr);

        charger_event_t evt = { .type = EVT_WIFI_CHANGED };
        event_bus_publish(&evt);
        ESP_LOGI(TAG, "STA connected, IP: %s  (%s.local)", s_ip_str, s_hostname);
    }
}

// ── HTTP server ───────────────────────────────────────────────────────────────

// 每條新連線都關閉 Nagle。
// esp_http_server 送回應時，標頭與主體是分開兩次 send()。Nagle 會扣住第二個
// 小分段，等第一段被 ACK 才送 —— 小回應（/config 527B、/history 157B）因此要等
// lwIP 慢速計時器才沖出去，實測固定慢 1.4 秒；而 /status（1528B，超過 MSS）
// 因為分段是滿的可以立即送出，反而不受影響，形成「大的快、小的慢」的怪現象。
static esp_err_t http_sock_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    int one = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        ESP_LOGW(TAG, "TCP_NODELAY failed on sock %d", sockfd);
    }
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 24;   // 目前註冊 18 個，留餘裕
    cfg.recv_wait_timeout = 30;   // allow slow WiFi during firmware upload
    cfg.open_fn           = http_sock_open;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_register_uri_handler(s_server, &s_uri_get_root);
    httpd_register_uri_handler(s_server, &s_uri_get_control);
    httpd_register_uri_handler(s_server, &s_uri_get_devices);
    httpd_register_uri_handler(s_server, &s_uri_get_status);
    httpd_register_uri_handler(s_server, &s_uri_get_config);
    httpd_register_uri_handler(s_server, &s_uri_post_config);
    httpd_register_uri_handler(s_server, &s_uri_post_start);
    httpd_register_uri_handler(s_server, &s_uri_post_stop);
    httpd_register_uri_handler(s_server, &s_uri_post_ota);
    httpd_register_uri_handler(s_server, &s_uri_post_ota_upload);
    httpd_register_uri_handler(s_server, &s_uri_get_manifest);
    httpd_register_uri_handler(s_server, &s_uri_get_sw);
    httpd_register_uri_handler(s_server, &s_uri_get_icon);
    httpd_register_uri_handler(s_server, &s_uri_get_wifi_scan);
    httpd_register_uri_handler(s_server, &s_uri_get_history);
    httpd_register_uri_handler(s_server, &s_uri_get_trace);
    httpd_register_uri_handler(s_server, &s_uri_get_tracelog);
    httpd_register_uri_handler(s_server, &s_uri_post_notify_test);
    httpd_register_uri_handler(s_server, &s_uri_post_psu_pair);
    httpd_register_uri_handler(s_server, &s_uri_get_mqtt_link);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t network_svc_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    const charger_config_t *cfg = config_svc_get();

    // 每台唯一的識別字串，mDNS 主機名與 AP SSID 都由它衍生
    snprintf(s_hostname, sizeof(s_hostname), "tes-%s",         cfg->device_id);
    snprintf(s_ap_ssid,  sizeof(s_ap_ssid),  "TES-Charger-%s", cfg->device_id);

    // 兩種情況都走 AP：沒有 SSID（首次設定），或使用者主動關掉 STA。
    // 後者保留 SSID／密碼不清除 —— 開關再打開就能直接連回去。
    if (cfg->wifi_ssid[0] == '\0' || !cfg->sta_enabled) {
        // No SSID: start open AP for initial WiFi configuration via POST /config
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();   // STA interface needed for /wifi/scan
        wifi_config_t ap_cfg = {
            .ap = {
                .channel        = 1,
                .authmode       = WIFI_AUTH_OPEN,
                .max_connection = 4,
            }
        };
        size_t ssid_len = strlen(s_ap_ssid);
        memcpy(ap_cfg.ap.ssid, s_ap_ssid, ssid_len);
        ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // APSTA enables scanning
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        s_ap_mode = true;
        ESP_LOGI(TAG, "no SSID — AP+STA mode: %s (open)", s_ap_ssid);
    } else {
        esp_netif_create_default_wifi_sta();
        wifi_config_t wcfg = {};
        strncpy((char *)wcfg.sta.ssid,     cfg->wifi_ssid, sizeof(wcfg.sta.ssid) - 1);
        strncpy((char *)wcfg.sta.password, cfg->wifi_pass,  sizeof(wcfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
        ESP_LOGI(TAG, "STA mode: SSID=%s", cfg->wifi_ssid);
    }
    return ESP_OK;
}

void network_svc_start(void)
{
    start_http_server();
    esp_wifi_start();

    // 關閉 WiFi 省電。ESP-IDF 的 STA 預設是 WIFI_PS_MIN_MODEM，無線電只在
    // 每 3 個 beacon（約 307ms）醒來一次，每次 TCP 往返都要等，網頁會慢到像離線。
    // 這台裝置是市電供電的固定設備，省那點電流沒有意義。
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(ps));
    }

    if (!s_ap_mode) {
        esp_wifi_connect();
    }
}
