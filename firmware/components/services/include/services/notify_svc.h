#pragma once
#include <esp_err.h>

esp_err_t notify_svc_init(void);
void      task_notify(void *arg);
esp_err_t notify_svc_send(const char *url, const char *title, const char *message, int priority);
