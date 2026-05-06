#pragma once
#include <esp_err.h>
#include <stdbool.h>

esp_err_t mqtt_svc_init      (void);
void      task_mqtt          (void *arg);
bool      mqtt_svc_is_connected(void);
