#include "hal/hal_uart.h"
#include <esp_log.h>

static const char *TAG = "hal_uart";

esp_err_t hal_uart_psu_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = HAL_UART_PSU_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(HAL_UART_PSU_PORT, &cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "param_config: %s", esp_err_to_name(ret)); return ret; }

    ret = uart_set_pin(HAL_UART_PSU_PORT,
                       PIN_UART_PSU_TX, PIN_UART_PSU_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "set_pin: %s", esp_err_to_name(ret)); return ret; }

    // 使用 event queue = NULL，RX buffer 256 bytes
    ret = uart_driver_install(HAL_UART_PSU_PORT, 256, 256, 0, NULL, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(ret)); }
    return ret;
}

int hal_uart_psu_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(HAL_UART_PSU_PORT, buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

int hal_uart_psu_write(const uint8_t *buf, size_t len)
{
    return uart_write_bytes(HAL_UART_PSU_PORT, (const char *)buf, len);
}
