#include "drivers/psu_driver.h"
#include "hal/hal_uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PSU_TIMEOUT_TICKS  300   // 300 × 10ms = 3 s with no valid frame → disconnect

static const char *TAG = "psu_driver";

static psu_status_t s_status   = { .voltage = 0.f, .current = 0.f, .connected = false };
static char         s_rx_buf[64];
static int          s_rx_len   = 0;
static uint32_t     s_poll_ticks       = 0;
static uint32_t     s_last_valid_ticks = 0;

esp_err_t psu_driver_init(void)
{
    return ESP_OK;   // UART already initialised by hal_uart_psu_init()
}

void psu_driver_poll(void)
{
    s_poll_ticks++;

    uint8_t byte;
    while (hal_uart_psu_read(&byte, 1, 0) == 1) {
        if (byte == '\n') {
            s_rx_buf[s_rx_len] = '\0';
            float v = 0.f, a = 0.f;
            if (sscanf(s_rx_buf, "V=%f,I=%f", &v, &a) == 2) {
                s_status.voltage         = v;
                s_status.current         = a;
                s_status.connected       = true;
                s_last_valid_ticks       = s_poll_ticks;
            } else {
                ESP_LOGW(TAG, "bad frame: %s", s_rx_buf);
            }
            s_rx_len = 0;
        } else if (s_rx_len < (int)(sizeof(s_rx_buf) - 1)) {
            s_rx_buf[s_rx_len++] = (char)byte;
        } else {
            s_rx_len = 0;   // overflow — discard and restart
        }
    }

    // Disconnect if no valid frame received within timeout
    if (s_status.connected &&
        (s_poll_ticks - s_last_valid_ticks) >= PSU_TIMEOUT_TICKS) {
        s_status.connected = false;
        s_status.voltage   = 0.f;
        s_status.current   = 0.f;
        ESP_LOGW(TAG, "PSU timeout — marking disconnected");
    }
}

void psu_driver_set_voltage(float v)
{
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "SET:V=%.1f\n", v);
    hal_uart_psu_write((const uint8_t *)buf, (size_t)len);
}

void psu_driver_set_current(float a)
{
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "SET:I=%.1f\n", a);
    hal_uart_psu_write((const uint8_t *)buf, (size_t)len);
}

psu_status_t psu_driver_get_status(void)
{
    return s_status;
}
