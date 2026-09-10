#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <driver/gpio.h>
#include "tes_protocol/tes_types.h"   // can_bus_state_t

// CAN Bus 腳位（對應 V2 Config.h）
#define PIN_TWAI_TX     GPIO_NUM_17
#define PIN_TWAI_RX     GPIO_NUM_18

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_frame_t;

esp_err_t can_driver_init(void);

// 發送：timeout_ms = 0 為非阻塞嘗試
esp_err_t can_driver_send(const can_frame_t *frame, uint32_t timeout_ms);

// 接收：阻塞等待 timeout_ms，無幀返回 false
bool      can_driver_receive(can_frame_t *frame, uint32_t timeout_ms);

// 檢查 TWAI alerts 並在 bus-off 時自動復歸。非阻塞，需由某個 task 定期呼叫
// （目前是 task_can_rx，每次 receive 逾時後）。沒有這個呼叫的話，一旦進入
// bus-off，CAN 會保持停擺直到重新開機。
void      can_driver_service(void);

// TWAI 控制器健康度。bus_err / rx_err 持續上升通常代表接線、終端電阻或
// baud rate 有問題 —— 只看「有沒有收到幀」看不出來這類半死不活的狀態。
typedef struct {
    uint8_t  state;        // can_bus_state_t
    uint32_t tx_err;
    uint32_t rx_err;
    uint32_t arb_lost;
    uint32_t bus_err;
    uint32_t rx_missed;
} can_health_t;

void      can_driver_get_health(can_health_t *out);
