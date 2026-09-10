#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

// WiFi + HTTP server service.
// When no SSID configured: opens "TES-Charger-<id>" AP at 192.168.4.1.
// When SSID configured: STA mode + mDNS hostname tes-<id>.local
//                       (plus a "tes-charger.local" alias for single-device setups).
// <id> = last 3 bytes of the MAC, so two units on one LAN never collide.
// REST API: GET /status, GET /config, POST /config, POST /start, POST /stop.

esp_err_t network_svc_init        (void);
void      network_svc_start       (void);
bool      network_svc_is_connected(void);
bool      network_svc_is_ap_mode  (void);
void      network_svc_get_ip_str  (char *buf, size_t len);

// 本機唯一的 mDNS 主機名（不含 .local），例如 "tes-a1b2c3"
void      network_svc_get_hostname(char *buf, size_t len);
