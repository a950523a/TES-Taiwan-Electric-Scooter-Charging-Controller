#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <driver/gpio.h>

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
