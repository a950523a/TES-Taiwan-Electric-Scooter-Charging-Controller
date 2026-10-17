#pragma once
#include <esp_err.h>

esp_err_t notify_svc_init(void);
void      task_notify(void *arg);
esp_err_t notify_svc_send(const char *url, const char *title, const char *message, int priority);
// notify_url 與 App 推播（push_svc）都送；兩邊都沒設定回 ESP_ERR_NOT_FOUND。
// 每支手機收它在 App 選的語言；notify_url（webhook／ntfy）一律中文，跟以前一樣。
esp_err_t notify_svc_broadcast(const char *title_zh, const char *msg_zh,
                               const char *title_en, const char *msg_en, int priority);
