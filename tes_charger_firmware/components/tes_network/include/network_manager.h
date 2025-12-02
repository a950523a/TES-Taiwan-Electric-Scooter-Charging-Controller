#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化網路服務 (WiFi + WebServer)
void network_init(void);

// 重置 WiFi 設定並重啟 (恢復 AP 模式)
void network_reset_config(void);

#ifdef __cplusplus
}
#endif