#include "network_manager.h"
#include <string.h>
#include <sys/param.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include <unistd.h> // for unlink
#include "mdns.h"

static const char *TAG = "NET_MGR";

// AP 模式預設設定
#define DEFAULT_AP_SSID "TES_Charger_V3"
#define DEFAULT_AP_PASS "12345678"

static httpd_handle_t server = NULL;

// --- Helper: NVS 讀寫 ---
static void save_wifi_cred(const char *ssid, const char *pass) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_set_str(my_handle, "ssid", ssid);
    nvs_set_str(my_handle, "pass", pass);
    nvs_commit(my_handle);
    nvs_close(my_handle);
}

static bool load_wifi_cred(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return false;
    
    if (nvs_get_str(my_handle, "ssid", ssid, &ssid_len) != ESP_OK) {
        nvs_close(my_handle);
        return false;
    }
    nvs_get_str(my_handle, "pass", pass, &pass_len);
    nvs_close(my_handle);
    return true;
}

// --- Handler: 掃描 WiFi (/api/scan) ---
static esp_err_t api_scan_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = { .show_hidden = true };
    esp_wifi_scan_start(&scan_config, true); // 阻塞式掃描

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(item, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(root, item);
    }
    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// --- Handler: 連接 WiFi (/api/connect) ---
static esp_err_t api_connect_handler(httpd_req_t *req) {
    char buf[200];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "pass");

    if (ssid && cJSON_IsString(ssid)) {
        save_wifi_cred(ssid->valuestring, pass ? pass->valuestring : "");
        httpd_resp_send(req, "Saved. Rebooting...", HTTPD_RESP_USE_STRLEN);
        
        // 延遲重啟
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        httpd_resp_send_500(req);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// --- Handler: 通用檔案上傳 (OTA / Script) ---
static esp_err_t upload_handler(httpd_req_t *req) {
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    FILE *fp = NULL;
    bool is_firmware = (strstr(req->uri, "fw") != NULL);

    if (is_firmware) {
        update_partition = esp_ota_get_next_update_partition(NULL);
        if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle) != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "OTA Update Started...");
    } else {
        // Script Update: 覆蓋 main.js
        unlink("/fs/main.js"); // 刪除舊檔
        fp = fopen("/fs/main.js", "wb");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open file for writing");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Script Update Started...");
    }

    char buf[1024];
    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        if ((received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }

        if (is_firmware) {
            esp_ota_write(update_handle, buf, received);
        } else {
            fwrite(buf, 1, received, fp);
        }
        remaining -= received;
    }

    if (is_firmware) {
        esp_ota_end(update_handle);
        esp_ota_set_boot_partition(update_partition);
        ESP_LOGI(TAG, "OTA Complete. Rebooting...");
        httpd_resp_send(req, "Firmware Updated. Rebooting...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        fclose(fp);
        ESP_LOGI(TAG, "Script Updated.");
        // 可以選擇重啟 JS 引擎，或直接重開機
        httpd_resp_send(req, "Script Updated. Rebooting...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    return ESP_OK;
}

// --- Handler: 靜態網頁 (/fs/index.html) ---
static esp_err_t index_handler(httpd_req_t *req) {
    FILE *f = fopen("/fs/index.html", "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunksize);
    }
    httpd_resp_send_chunk(req, NULL, 0); // End
    fclose(f);
    return ESP_OK;
}

void start_mdns(void) {
    ESP_ERROR_CHECK(mdns_init());
    // 設定主機名稱：tes-charger.local
    ESP_ERROR_CHECK(mdns_hostname_set("tes-charger"));
    ESP_ERROR_CHECK(mdns_instance_name_set("TES Charger V3"));

    // 註冊 HTTP 服務 (這樣手機的瀏覽器可以自動發現)
    mdns_txt_item_t serviceTxtData[1] = {
        {"board", "esp32s3"}
    };
    ESP_ERROR_CHECK(mdns_service_add("TES Charger", "_http", "_tcp", 80, serviceTxtData, 1));
    
    ESP_LOGI(TAG, "mDNS started. Access at http://tes-charger.local");
}

// --- Server Setup ---
void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192; // 加大 Stack 避免上傳時崩潰

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_register_uri_handler(server, &uri_index);

        httpd_uri_t uri_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_handler };
        httpd_register_uri_handler(server, &uri_scan);

        httpd_uri_t uri_conn = { .uri = "/api/connect", .method = HTTP_POST, .handler = api_connect_handler };
        httpd_register_uri_handler(server, &uri_conn);

        httpd_uri_t uri_up_fw = { .uri = "/api/upload_fw", .method = HTTP_POST, .handler = upload_handler };
        httpd_register_uri_handler(server, &uri_up_fw);

        httpd_uri_t uri_up_js = { .uri = "/api/upload_js", .method = HTTP_POST, .handler = upload_handler };
        httpd_register_uri_handler(server, &uri_up_js);
    }
}

// --- WiFi Init ---
void network_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    char ssid[32] = {0};
    char pass[64] = {0};

    if (load_wifi_cred(ssid, sizeof(ssid), pass, sizeof(pass))) {
        // STA Mode
        wifi_config_t wifi_config = {0};
        strcpy((char *)wifi_config.sta.ssid, ssid);
        strcpy((char *)wifi_config.sta.password, pass);
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());
        ESP_LOGI(TAG, "Connecting to %s...", ssid);
    } else {
        // AP Mode
        wifi_config_t wifi_config = {
            .ap = {
                .ssid = DEFAULT_AP_SSID,
                .ssid_len = strlen(DEFAULT_AP_SSID),
                .password = DEFAULT_AP_PASS,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
        };
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "AP Mode Started: %s", DEFAULT_AP_SSID);
    }
    start_mdns(); 

    start_webserver();
}

void network_reset_config(void) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_erase_key(my_handle, "ssid");
    nvs_erase_key(my_handle, "pass");
    nvs_commit(my_handle);
    nvs_close(my_handle);
    esp_restart();
}

