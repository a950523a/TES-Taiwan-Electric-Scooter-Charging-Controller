#include "services/network_svc.h"
#include "services/config_svc.h"
#include "services/event_bus.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "network_svc";

static httpd_handle_t s_server  = NULL;
static bool           s_connected = false;

// ── WiFi event handler ───────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_wifi_connect();  // auto-reconnect
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        charger_event_t evt = { .type = EVT_WIFI_CHANGED };
        event_bus_publish(&evt);
        ESP_LOGI(TAG, "connected");
    }
}

// ── HTTP handlers (stubs) ────────────────────────────────────────────────────

static esp_err_t handle_status(httpd_req_t *req)
{
    // TODO: serialise g_snapshot to JSON via cJSON
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"state\":\"stub\"}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_status = {
    .uri = "/status", .method = HTTP_GET, .handler = handle_status
};

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size      = 8192;
    if (httpd_start(&s_server, &cfg) == ESP_OK) {
        httpd_register_uri_handler(s_server, &s_uri_status);
        ESP_LOGI(TAG, "HTTP server started");
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

esp_err_t network_svc_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    const charger_config_t *cfg = config_svc_get();
    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no SSID configured — captive portal TODO");
        return ESP_OK;
    }

    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid,     cfg->wifi_ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, cfg->wifi_pass,  sizeof(wcfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    return ESP_OK;
}

void network_svc_start(void)
{
    esp_wifi_start();
    esp_wifi_connect();
    start_http_server();
}

bool network_svc_is_connected(void)
{
    return s_connected;
}
