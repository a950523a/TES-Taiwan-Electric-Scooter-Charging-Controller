// network_svc.c — WiFi + REST API
//
// No SSID configured → AP mode, broadcasts "TES-Charger" (open), IP 192.168.4.1
// SSID configured    → STA mode, auto-reconnect, mDNS hostname "tes-charger.local"
// HTTP server runs in both modes on port 80.

#include "services/network_svc.h"
#include "services/config_svc.h"
#include "services/event_bus.h"
#include "services/ota_svc.h"
#include "services/notify_svc.h"
#include "services/log_svc.h"
#include "tes_protocol/tes_types.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
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

// ── GET / (embedded web UI) ───────────────────────────────────────────────────

extern const uint8_t s_index_html_start[]   asm("_binary_index_html_start");
extern const uint8_t s_index_html_end[]     asm("_binary_index_html_end");
extern const uint8_t s_manifest_json_start[] asm("_binary_manifest_json_start");
extern const uint8_t s_manifest_json_end[]   asm("_binary_manifest_json_end");
extern const uint8_t s_sw_js_start[]        asm("_binary_sw_js_start");
extern const uint8_t s_sw_js_end[]          asm("_binary_sw_js_end");
extern const uint8_t s_icon_svg_start[]     asm("_binary_icon_svg_start");
extern const uint8_t s_icon_svg_end[]       asm("_binary_icon_svg_end");

static esp_err_t handle_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400, stale-if-error=604800");
    httpd_resp_send(req, (const char *)s_index_html_start,
                    s_index_html_end - s_index_html_start);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get_root = {
    .uri = "/", .method = HTTP_GET, .handler = handle_get_root
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
    cJSON_AddNumberToObject(root, "soc",               snap.soc);
    cJSON_AddNumberToObject(root, "target_soc",        snap.target_soc);
    cJSON_AddBoolToObject  (root, "fault",             snap.fault_latched);
    cJSON_AddBoolToObject  (root, "charge_complete",   snap.charge_complete);
    cJSON_AddBoolToObject  (root, "timer_running",     snap.timer_running);
    cJSON_AddNumberToObject(root, "elapsed_seconds",   snap.elapsed_seconds);
    cJSON_AddNumberToObject(root, "remaining_seconds", snap.remaining_seconds);
    cJSON_AddNumberToObject(root, "fault_flags",       snap.last_fault_flags);
    cJSON_AddBoolToObject  (root, "wifi_connected",    s_connected);
    cJSON_AddStringToObject(root, "ip",                s_ip_str);

    // OTA 狀態
    ota_state_t ota_st = ota_svc_get_state();
    cJSON_AddBoolToObject  (root, "ota_running",  ota_st == OTA_STATE_RUNNING);
    cJSON_AddNumberToObject(root, "ota_progress", ota_svc_progress_pct());
    if (ota_st == OTA_STATE_ERROR)
        cJSON_AddStringToObject(root, "ota_error", ota_svc_get_error());

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
    cJSON_AddBoolToObject  (root, "auto_voltage",   cfg->auto_voltage);
    cJSON_AddNumberToObject(root, "max_voltage",    (double)cfg->max_voltage_01v / 10.0);
    cJSON_AddNumberToObject(root, "max_current",    (double)cfg->max_current_01a / 10.0);
    cJSON_AddNumberToObject(root, "target_soc",     cfg->target_soc);
    cJSON_AddNumberToObject(root, "stop_mode",      (int)cfg->stop_mode);
    cJSON_AddNumberToObject(root, "stop_voltage",   (double)cfg->stop_voltage_01v / 10.0);
    cJSON_AddStringToObject(root, "wifi_ssid",      cfg->wifi_ssid);
    cJSON_AddBoolToObject  (root, "beacon",         cfg->beacon_unlocked);
    cJSON_AddStringToObject(root, "notify_url",     cfg->notify_url);

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
        if (m >= 0 && m <= 1) { new_stop_mode = (uint8_t)m; stop_changed = true; }
    }
    item = cJSON_GetObjectItem(root, "stop_voltage");
    if (cJSON_IsNumber(item)) {
        int v = (int)(item->valuedouble * 10.0 + 0.5);
        if (v >= 400 && v <= 1200) { new_stop_voltage = (uint16_t)v; stop_changed = true; }
    }

    char new_notify_url[128] = {0};
    bool notify_url_changed = false;
    item = cJSON_GetObjectItem(root, "notify_url");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(new_notify_url, item->valuestring, sizeof(new_notify_url) - 1);
        notify_url_changed = true;
    }

    cJSON_Delete(root);

    if (charging_changed)     config_svc_set_charging(new_voltage, new_current, new_soc);
    if (wifi_changed)         config_svc_set_wifi(new_ssid, new_pass);
    if (beacon_changed)       config_svc_set_beacon(new_beacon);
    if (auto_voltage_changed) config_svc_set_auto_voltage(new_auto_voltage);
    if (notify_url_changed)   config_svc_set_notify_url(new_notify_url);
    if (stop_changed)         config_svc_set_stop((stop_mode_t)new_stop_mode, new_stop_voltage);

    if (wifi_changed)
        ESP_LOGI(TAG, "WiFi credentials updated — reboot to connect");

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"reboot to apply wifi changes\"}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_post_config = {
    .uri = "/config", .method = HTTP_POST, .handler = handle_post_config
};

// ── POST /start ───────────────────────────────────────────────────────────────

static esp_err_t handle_post_start(httpd_req_t *req)
{
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
    char url[256] = "";

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
        cJSON_AddNumberToObject(obj, "duration_s",  buf[i].duration_s);
        cJSON_AddNumberToObject(obj, "energy_wh",   (double)buf[i].energy_wh);
        cJSON_AddNumberToObject(obj, "soc_start",   buf[i].soc_start);
        cJSON_AddNumberToObject(obj, "soc_end",     buf[i].soc_end);
        cJSON_AddNumberToObject(obj, "stop_reason", buf[i].stop_reason);
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

// ── POST /notify/test ─────────────────────────────────────────────────────────

static esp_err_t handle_post_notify_test(httpd_req_t *req)
{
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

// ── WiFi event handler ────────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_START) {
            // AP mode is up — our IP is always 192.168.4.1
            strncpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str) - 1);
            ESP_LOGI(TAG, "AP \"TES-Charger\" started @ %s", s_ip_str);
            if (!s_mdns_ok) {
                mdns_init();
                mdns_hostname_set("tes-charger");
                mdns_instance_name_set("TES Charger");
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                s_mdns_ok = true;
                ESP_LOGI(TAG, "mDNS: tes-charger.local -> %s", s_ip_str);
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected  = false;
            s_ip_str[0]  = '\0';
            if (!s_ap_mode) esp_wifi_connect();  // auto-reconnect (not in AP-only setup mode)
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_connected = true;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));

        if (!s_mdns_ok) {
            mdns_init();
            mdns_hostname_set("tes-charger");
            mdns_instance_name_set("TES Charger");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            s_mdns_ok = true;
            ESP_LOGI(TAG, "mDNS: tes-charger.local -> %s", s_ip_str);
        }

        charger_event_t evt = { .type = EVT_WIFI_CHANGED };
        event_bus_publish(&evt);
        ESP_LOGI(TAG, "STA connected, IP: %s", s_ip_str);
    }
}

// ── HTTP server ───────────────────────────────────────────────────────────────

static void start_http_server(void)
{
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 16;
    cfg.recv_wait_timeout = 30;   // allow slow WiFi during firmware upload

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_register_uri_handler(s_server, &s_uri_get_root);
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
    httpd_register_uri_handler(s_server, &s_uri_post_notify_test);
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
    if (cfg->wifi_ssid[0] == '\0') {
        // No SSID: start open AP for initial WiFi configuration via POST /config
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();   // STA interface needed for /wifi/scan
        wifi_config_t ap_cfg = {
            .ap = {
                .ssid           = "TES-Charger",
                .ssid_len       = 11,
                .channel        = 1,
                .authmode       = WIFI_AUTH_OPEN,
                .max_connection = 4,
            }
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // APSTA enables scanning
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        s_ap_mode = true;
        ESP_LOGI(TAG, "no SSID — AP+STA mode: TES-Charger (open)");
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
    if (!s_ap_mode) {
        esp_wifi_connect();
    }
}
