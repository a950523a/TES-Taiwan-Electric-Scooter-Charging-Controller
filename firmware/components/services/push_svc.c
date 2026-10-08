#include "services/push_svc.h"
#include "hal/hal_nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "push_svc";

#define NVS_NS        "tes_cfg"
#define NVS_KEY_TOKS  "push_toks"     // blob：PUSH_MAX_TOKENS × PUSH_TOKEN_LEN
#define EXPO_PUSH_URL "https://exp.host/--/api/v2/push/send"
// App 端建立的 Android 通知頻道（TurtlePower-App src/core/push.ts 的 CHANNEL_ID），兩邊要一致
#define ANDROID_CHANNEL "charging"

static char              s_tokens[PUSH_MAX_TOKENS][PUSH_TOKEN_LEN];
static SemaphoreHandle_t s_lock;

static esp_err_t save_locked(void)
{
    return hal_nvs_set_blob(NVS_NS, NVS_KEY_TOKS, s_tokens, sizeof(s_tokens));
}

esp_err_t push_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    size_t len = sizeof(s_tokens);
    if (hal_nvs_get_blob(NVS_NS, NVS_KEY_TOKS, s_tokens, &len) != ESP_OK || len != sizeof(s_tokens)) {
        memset(s_tokens, 0, sizeof(s_tokens));
    }
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) s_tokens[i][PUSH_TOKEN_LEN - 1] = '\0';
    ESP_LOGI(TAG, "%d push token(s)", push_svc_count());
    return ESP_OK;
}

// ExponentPushToken[xxxx] 或 ExpoPushToken[xxxx]。token 會原樣放進 JSON 字串，
// 所以只收英數與 - _，不必再做跳脫。
static bool token_valid(const char *t)
{
    const char *body;
    if (strncmp(t, "ExponentPushToken[", 18) == 0)  body = t + 18;
    else if (strncmp(t, "ExpoPushToken[", 14) == 0) body = t + 14;
    else return false;
    size_t n = strlen(t);
    if (n >= PUSH_TOKEN_LEN || t[n - 1] != ']') return false;
    if (body >= t + n - 1) return false;   // 括號裡是空的
    for (const char *p = body; p < t + n - 1; p++) {
        char c = *p;
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

esp_err_t push_svc_add(const char *token)
{
    if (!token || !token_valid(token)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int free_slot = -1;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        if (strcmp(s_tokens[i], token) == 0) {
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        if (free_slot < 0 && s_tokens[i][0] == '\0') free_slot = i;
    }
    esp_err_t r = ESP_ERR_NO_MEM;
    if (free_slot >= 0) {
        strcpy(s_tokens[free_slot], token);
        r = save_locked();
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t push_svc_remove(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        if (s_tokens[i][0] != '\0' && strcmp(s_tokens[i], token) == 0) {
            s_tokens[i][0] = '\0';
            r = save_locked();
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

int push_svc_count(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) n += s_tokens[i][0] != '\0';
    xSemaphoreGive(s_lock);
    return n;
}

typedef enum { SEND_OK, SEND_FAIL, SEND_GONE } send_result_t;

// 緩衝區放 heap：task_notify 只有 6 KB 堆疊，TLS 握手本身就要用掉不少
typedef struct {
    char toks[PUSH_MAX_TOKENS][PUSH_TOKEN_LEN];
    char json[320];
    char resp[192];
} send_buf_t;

// 一支手機一個請求：回應很小，看得出是哪一支失效，不用解析陣列。
static send_result_t send_one(send_buf_t *b, const char *token, const char *title, const char *body)
{
    int n = snprintf(b->json, sizeof(b->json),
                     "{\"to\":\"%s\",\"title\":\"%s\",\"body\":\"%s\",\"priority\":\"high\","
                     "\"sound\":\"default\",\"channelId\":\"" ANDROID_CHANNEL "\"}",
                     token, title, body);
    if (n < 0 || n >= (int)sizeof(b->json)) return SEND_FAIL;

    esp_http_client_config_t cfg = {
        .url               = EXPO_PUSH_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return SEND_FAIL;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");

    send_result_t result = SEND_FAIL;
    if (esp_http_client_open(c, n) == ESP_OK && esp_http_client_write(c, b->json, n) == n) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int got = esp_http_client_read_response(c, b->resp, sizeof(b->resp) - 1);
        b->resp[got > 0 ? got : 0] = '\0';
        // 成功：{"data":{"status":"ok","id":"..."}}
        // 失效：{"data":{"status":"error",...,"details":{"error":"DeviceNotRegistered"}}}
        if (strstr(b->resp, "DeviceNotRegistered")) {
            result = SEND_GONE;
        } else if (status == 200 && strstr(b->resp, "\"ok\"") && !strstr(b->resp, "\"error\"")) {
            result = SEND_OK;
        } else {
            ESP_LOGW(TAG, "expo HTTP %d: %s", status, b->resp);
        }
    } else {
        ESP_LOGW(TAG, "expo request failed");
    }
    esp_http_client_cleanup(c);
    return result;
}

esp_err_t push_svc_send_all(const char *title, const char *body)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    send_buf_t *b = malloc(sizeof(*b));
    if (!b) return ESP_ERR_NO_MEM;
    // 先複製出來，送的時候（每支可能好幾秒）不要握著鎖
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(b->toks, s_tokens, sizeof(b->toks));
    xSemaphoreGive(s_lock);

    int sent = 0, tried = 0;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        const char *tok = b->toks[i];
        if (tok[0] == '\0') continue;
        tried++;
        send_result_t r = send_one(b, tok, title, body);
        if (r == SEND_OK) {
            sent++;
        } else if (r == SEND_GONE) {
            ESP_LOGI(TAG, "token %d no longer registered — removed", i);
            push_svc_remove(tok);
        }
    }
    free(b);
    if (tried == 0) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "push \"%s\": %d/%d", title, sent, tried);
    return sent > 0 ? ESP_OK : ESP_FAIL;
}
