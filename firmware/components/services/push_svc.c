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
#include <time.h>

static const char *TAG = "push_svc";

#define NVS_NS          "tes_cfg"
#define NVS_KEY_PHONES  "push_v2"       // blob：PUSH_MAX_TOKENS × phone_t
#define NVS_KEY_TOKS_V1 "push_toks"     // 舊格式（只有 token），開機時搬到 push_v2
#define EXPO_PUSH_URL   "https://exp.host/--/api/v2/push/send"
// App 端建立的 Android 通知頻道（TurtlePower-App src/core/push.ts 的 CHANNEL_ID），兩邊要一致
#define ANDROID_CHANNEL "charging"
// 資料推播（充電進度）的存活時間：晚到的進度沒有意義，過了就丟掉
#define DATA_TTL_S      300

typedef struct {
    char     token[PUSH_TOKEN_LEN];
    char     name[PUSH_NAME_LEN];
    uint32_t added;
} phone_t;

static phone_t           s_phones[PUSH_MAX_TOKENS];
static SemaphoreHandle_t s_lock;

static esp_err_t save_locked(void)
{
    return hal_nvs_set_blob(NVS_NS, NVS_KEY_PHONES, s_phones, sizeof(s_phones));
}

esp_err_t push_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    size_t len = sizeof(s_phones);
    if (hal_nvs_get_blob(NVS_NS, NVS_KEY_PHONES, s_phones, &len) != ESP_OK || len != sizeof(s_phones)) {
        memset(s_phones, 0, sizeof(s_phones));
        // 舊韌體只存 token：搬過來，名稱留空、時間未知
        char old[PUSH_MAX_TOKENS][PUSH_TOKEN_LEN];
        size_t olen = sizeof(old);
        if (hal_nvs_get_blob(NVS_NS, NVS_KEY_TOKS_V1, old, &olen) == ESP_OK && olen == sizeof(old)) {
            for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
                old[i][PUSH_TOKEN_LEN - 1] = '\0';
                strcpy(s_phones[i].token, old[i]);
            }
            save_locked();
            ESP_LOGI(TAG, "migrated push tokens to push_v2");
        }
    }
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        s_phones[i].token[PUSH_TOKEN_LEN - 1] = '\0';
        s_phones[i].name[PUSH_NAME_LEN - 1]   = '\0';
    }
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

// 名稱由使用者的手機提供：去掉 " \ 與控制字元，並在 UTF-8 字元邊界截斷，
// 存進去的就是可以直接放進 JSON 字串的內容。
static void sanitize_name(char *dst, const char *src)
{
    size_t k = 0;
    if (src) {
        for (const unsigned char *p = (const unsigned char *)src; *p; ) {
            size_t clen = (*p < 0x80) ? 1 : (*p >> 5) == 0x6 ? 2 : (*p >> 4) == 0xE ? 3 : (*p >> 3) == 0x1E ? 4 : 1;
            if (k + clen > PUSH_NAME_LEN - 1) break;
            if (clen == 1 && (*p < 0x20 || *p == '"' || *p == '\\' || *p >= 0x80)) { p++; continue; }
            bool whole = true;
            for (size_t j = 1; j < clen; j++) if ((p[j] & 0xC0) != 0x80) whole = false;
            if (!whole) { p++; continue; }
            memcpy(dst + k, p, clen);
            k += clen;
            p += clen;
        }
    }
    dst[k] = '\0';
}

static uint32_t now_epoch(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;   // 還沒對時（1970 年起算）就記 0
}

esp_err_t push_svc_add(const char *token, const char *name)
{
    if (!token || !token_valid(token)) return ESP_ERR_INVALID_ARG;
    char clean[PUSH_NAME_LEN];
    sanitize_name(clean, name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int free_slot = -1;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        if (strcmp(s_phones[i].token, token) == 0) {
            esp_err_t r = ESP_OK;
            if (clean[0] && strcmp(s_phones[i].name, clean) != 0) {
                strcpy(s_phones[i].name, clean);
                r = save_locked();
            }
            xSemaphoreGive(s_lock);
            return r;
        }
        if (free_slot < 0 && s_phones[i].token[0] == '\0') free_slot = i;
    }
    esp_err_t r = ESP_ERR_NO_MEM;
    if (free_slot >= 0) {
        strcpy(s_phones[free_slot].token, token);
        strcpy(s_phones[free_slot].name, clean);
        s_phones[free_slot].added = now_epoch();
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
        if (s_phones[i].token[0] != '\0' && strcmp(s_phones[i].token, token) == 0) {
            memset(&s_phones[i], 0, sizeof(s_phones[i]));
            r = save_locked();
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t push_svc_remove_slot(int slot)
{
    if (slot < 0 || slot >= PUSH_MAX_TOKENS) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = ESP_ERR_NOT_FOUND;
    if (s_phones[slot].token[0] != '\0') {
        memset(&s_phones[slot], 0, sizeof(s_phones[slot]));
        r = save_locked();
    }
    xSemaphoreGive(s_lock);
    return r;
}

int push_svc_count(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) n += s_phones[i].token[0] != '\0';
    xSemaphoreGive(s_lock);
    return n;
}

int push_svc_list(push_phone_info_t *out)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        const phone_t *p = &s_phones[i];
        if (p->token[0] == '\0') continue;
        push_phone_info_t *o = &out[n++];
        o->slot  = i;
        o->added = p->added;
        strcpy(o->name, p->name);
        // 括號內最後 6 碼：App 拿自己的 token 比對，認出「這支手機」
        size_t len = strlen(p->token);
        size_t end = len - 1;                    // ']' 的位置
        size_t start = end >= PUSH_TAIL_LEN - 1 ? end - (PUSH_TAIL_LEN - 1) : 0;
        memcpy(o->tail, p->token + start, end - start);
        o->tail[end - start] = '\0';
    }
    xSemaphoreGive(s_lock);
    return n;
}

typedef enum { SEND_OK, SEND_FAIL, SEND_GONE } send_result_t;

// 緩衝區放 heap：task_notify 只有 6 KB 堆疊，TLS 握手本身就要用掉不少
typedef struct {
    char toks[PUSH_MAX_TOKENS][PUSH_TOKEN_LEN];
    char json[512];
    char resp[192];
} send_buf_t;

// 一支手機一個請求：回應很小，看得出是哪一支失效，不用解析陣列。b->json 已填好。
static send_result_t post_one(send_buf_t *b, int n)
{
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

// 對每支登記的手機送一次。title 不為 NULL 是一般通知，否則是資料推播（data_json）。
static esp_err_t send_each(const char *title, const char *body, const char *data_json)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    send_buf_t *b = malloc(sizeof(*b));
    if (!b) return ESP_ERR_NO_MEM;
    // 先複製出來，送的時候（每支可能好幾秒）不要握著鎖
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) strcpy(b->toks[i], s_phones[i].token);
    xSemaphoreGive(s_lock);

    int sent = 0, tried = 0;
    for (int i = 0; i < PUSH_MAX_TOKENS; i++) {
        const char *tok = b->toks[i];
        if (tok[0] == '\0') continue;
        tried++;
        int n = title
            ? snprintf(b->json, sizeof(b->json),
                       "{\"to\":\"%s\",\"title\":\"%s\",\"body\":\"%s\",\"priority\":\"high\","
                       "\"sound\":\"default\",\"channelId\":\"" ANDROID_CHANNEL "\"}",
                       tok, title, body)
            : snprintf(b->json, sizeof(b->json),
                       "{\"to\":\"%s\",\"data\":%s,\"priority\":\"high\",\"ttl\":%d}",
                       tok, data_json, DATA_TTL_S);
        if (n < 0 || n >= (int)sizeof(b->json)) continue;
        send_result_t r = post_one(b, n);
        if (r == SEND_OK) {
            sent++;
        } else if (r == SEND_GONE) {
            ESP_LOGI(TAG, "token %d no longer registered — removed", i);
            push_svc_remove(tok);
        }
    }
    free(b);
    if (tried == 0) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "push \"%s\": %d/%d", title ? title : "(data)", sent, tried);
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t push_svc_send_all(const char *title, const char *body)
{
    return send_each(title, body, NULL);
}

esp_err_t push_svc_send_data_all(const char *data_json)
{
    return send_each(NULL, NULL, data_json);
}
