#pragma once
#include <esp_err.h>
#include <stdbool.h>

// 手機 App 推播（Expo Push Service）。
//
// App 在區網內把自己的 Expo 推播 token 登記到這台（POST /push），事件發生時
// 這裡直接 POST 到 https://exp.host/--/api/v2/push/send，不需要自己的伺服器。
// token 只能用來推播給那一支手機，所以外洩的後果有限；刻意不放進
// charger_config_t——那個結構會整包經 GET /config 送出去。

#define PUSH_MAX_TOKENS 4
#define PUSH_TOKEN_LEN  64    // 含 NUL；Expo token 約 41 字元

esp_err_t push_svc_init(void);

// 已登記就回 ESP_OK（不重複）；格式不對 ESP_ERR_INVALID_ARG；滿了 ESP_ERR_NO_MEM
esp_err_t push_svc_add(const char *token);
// 沒有這個 token 回 ESP_ERR_NOT_FOUND
esp_err_t push_svc_remove(const char *token);
int       push_svc_count(void);

// 送給所有登記的 token。至少一支成功回 ESP_OK；沒有 token 回 ESP_ERR_NOT_FOUND。
// Expo 回 DeviceNotRegistered（App 被移除、token 失效）的 token 會自動移除。
// 會做 HTTPS，只能在有足夠堆疊的任務裡呼叫（task_notify、httpd worker）。
esp_err_t push_svc_send_all(const char *title, const char *body);
