#pragma once
#include <esp_err.h>
#include <stdbool.h>

// WiFi + HTTP server service.
// Captive portal for first-time SSID/password setup.
// REST API: GET /status, POST /config, POST /start, POST /stop.

esp_err_t network_svc_init (void);
void      network_svc_start(void);
bool      network_svc_is_connected(void);
