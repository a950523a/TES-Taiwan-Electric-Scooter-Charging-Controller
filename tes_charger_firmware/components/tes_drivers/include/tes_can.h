#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


// 初始化 CAN
esp_err_t tes_can_init(void);

// 發送 CAN 訊息
// id: CAN ID (標準幀)
// data: 數據指針
// len: 數據長度 (0-8)
esp_err_t tes_can_send(uint32_t id, const uint8_t *data, uint8_t len);

// 接收 CAN 訊息 (非阻塞)
// id: 輸出 ID
// data: 輸出數據 buffer (至少 8 bytes)
// len: 輸出長度
// 回傳: true 代表有收到資料，false 代表沒資料
bool tes_can_receive(uint32_t *id, uint8_t *data, uint8_t *len);

void tes_can_check_status(void);

#ifdef __cplusplus
}
#endif