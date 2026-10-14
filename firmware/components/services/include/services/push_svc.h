#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// 手機 App 推播（Expo Push Service）。
//
// App 在區網內把自己的 Expo 推播 token 登記到這台（POST /push），事件發生時
// 這裡直接 POST 到 https://exp.host/--/api/v2/push/send，不需要自己的伺服器。
// token 只能用來推播給那一支手機，所以外洩的後果有限；刻意不放進
// charger_config_t——那個結構會整包經 GET /config 送出去。GET /push 只列名稱、
// 登記時間與 token 最後幾碼，讓 App 管理「哪些手機會收到通知」。

#define PUSH_MAX_TOKENS 4
#define PUSH_TOKEN_LEN  64    // 含 NUL；Expo token 約 41 字元
#define PUSH_NAME_LEN   32    // 含 NUL；UTF-8，存入前已去掉 " \ 與控制字元
#define PUSH_TAIL_LEN   7     // token 括號內最後 6 碼 + NUL

typedef struct {
    int      slot;
    char     name[PUSH_NAME_LEN];
    uint32_t added;               // Unix 秒；NTP 還沒對時就登記的是 0
    char     tail[PUSH_TAIL_LEN];
} push_phone_info_t;

esp_err_t push_svc_init(void);

// 已登記就只更新名稱；格式不對 ESP_ERR_INVALID_ARG；滿了 ESP_ERR_NO_MEM
esp_err_t push_svc_add(const char *token, const char *name);
// 沒有這個 token 回 ESP_ERR_NOT_FOUND
esp_err_t push_svc_remove(const char *token);
// 依 GET /push 的 slot 移除（給「移除別支手機」用，App 不知道別人的完整 token）
esp_err_t push_svc_remove_slot(int slot);
int       push_svc_count(void);
// 回傳筆數，out 至少要 PUSH_MAX_TOKENS 個
int       push_svc_list(push_phone_info_t *out);

// 送給所有登記的 token。至少一支成功回 ESP_OK；沒有 token 回 ESP_ERR_NOT_FOUND。
// Expo 回 DeviceNotRegistered（App 被移除、token 失效）的 token 會自動移除。
// 會做 HTTPS，只能在有足夠堆疊的任務裡呼叫（task_notify、httpd worker）。
esp_err_t push_svc_send_all(const char *title, const char *body);

// 只有資料、沒有標題內文的推播（App 在背景收到後自己更新通知，例如充電進度）。
// data_json 是一個 JSON 物件字串，呼叫端負責它是合法、免跳脫的 ASCII。
esp_err_t push_svc_send_data_all(const char *data_json);
