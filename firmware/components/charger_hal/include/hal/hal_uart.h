#pragma once
#include <esp_err.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <stdint.h>
#include <stddef.h>

// PSU UART（與 V2 PowerSupplyController 相同腳位）
#define HAL_UART_PSU_PORT   UART_NUM_1
#define PIN_UART_PSU_RX     GPIO_NUM_44
#define PIN_UART_PSU_TX     GPIO_NUM_43
#define HAL_UART_PSU_BAUD   115200

esp_err_t hal_uart_psu_init (void);
int       hal_uart_psu_read (uint8_t *buf, size_t max_len, uint32_t timeout_ms);
int       hal_uart_psu_write(const uint8_t *buf, size_t len);
