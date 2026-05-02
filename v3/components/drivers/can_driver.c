#include "drivers/can_driver.h"
#include "esp_log.h"
#include "driver/twai.h"

static const char *TAG = "can_driver";

esp_err_t can_driver_init(void)
{
    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(PIN_TWAI_TX, PIN_TWAI_RX, TWAI_MODE_NORMAL);
    g_cfg.rx_queue_len = 16;
    g_cfg.tx_queue_len = 8;

    twai_timing_config_t t_cfg = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
    }
    return ret;
}

esp_err_t can_driver_send(const can_frame_t *frame, uint32_t timeout_ms)
{
    twai_message_t msg = {
        .identifier      = frame->id,
        .data_length_code = frame->dlc,
        .extd            = 0,
        .rtr             = 0,
    };
    for (int i = 0; i < frame->dlc; i++) {
        msg.data[i] = frame->data[i];
    }
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return twai_transmit(&msg, ticks);
}

bool can_driver_receive(can_frame_t *frame, uint32_t timeout_ms)
{
    twai_message_t msg;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (twai_receive(&msg, ticks) != ESP_OK) {
        return false;
    }
    frame->id  = msg.identifier;
    frame->dlc = msg.data_length_code;
    for (int i = 0; i < msg.data_length_code; i++) {
        frame->data[i] = msg.data[i];
    }
    return true;
}
