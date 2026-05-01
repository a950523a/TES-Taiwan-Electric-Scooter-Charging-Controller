#pragma once
#include <esp_err.h>
#include <stdbool.h>

// OTA update service (ESP-IDF esp_https_ota).
// Triggered via REST POST /ota with JSON {"url": "..."}.
// Progress reported via event_bus EVT_OTA_PROGRESS / EVT_OTA_COMPLETE.

esp_err_t ota_svc_start(const char *url);
bool      ota_svc_is_running(void);
int       ota_svc_progress_pct(void);  // 0-100
