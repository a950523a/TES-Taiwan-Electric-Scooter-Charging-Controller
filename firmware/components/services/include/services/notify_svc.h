#pragma once
#include <esp_err.h>

esp_err_t notify_svc_init(void);
void      task_notify(void *arg);
esp_err_t notify_svc_send(const char *url, const char *title, const char *message, int priority);
// notify_url 與 App 推播（push_svc）都送；兩邊都沒設定回 ESP_ERR_NOT_FOUND
esp_err_t notify_svc_broadcast(const char *title, const char *message, int priority);
