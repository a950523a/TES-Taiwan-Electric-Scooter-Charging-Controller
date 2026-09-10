#include "drivers/can_driver.h"
#include "esp_log.h"
#include "driver/twai.h"

static const char *TAG = "can_driver";

esp_err_t can_driver_init(void)
{
    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(PIN_TWAI_TX, PIN_TWAI_RX, TWAI_MODE_NORMAL);
    g_cfg.rx_queue_len = 16;
    g_cfg.tx_queue_len = 8;
    // 沒有車端在線時（例如 auto_start 在 IDLE 也持續廣播 0x508/0x509）幀不會被
    // ACK，控制器會一路走到 bus-off。開啟 alerts 讓 can_driver_service() 能復歸。
    g_cfg.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR;

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

void can_driver_get_health(can_health_t *out)
{
    if (!out) return;
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) {
        out->state = CAN_BUS_STOPPED;
        out->tx_err = out->rx_err = out->arb_lost = out->bus_err = out->rx_missed = 0;
        return;
    }
    switch (s.state) {
    case TWAI_STATE_RUNNING:    out->state = CAN_BUS_RUNNING;    break;
    case TWAI_STATE_BUS_OFF:    out->state = CAN_BUS_OFF;        break;
    case TWAI_STATE_RECOVERING: out->state = CAN_BUS_RECOVERING; break;
    default:                    out->state = CAN_BUS_STOPPED;    break;
    }
    out->tx_err     = s.tx_error_counter;
    out->rx_err     = s.rx_error_counter;
    out->arb_lost   = s.arb_lost_count;
    out->bus_err    = s.bus_error_count;
    out->rx_missed  = s.rx_missed_count;
}

void can_driver_service(void)
{
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) return;   // 非阻塞：沒有 alert 就直接返回

    if (alerts & TWAI_ALERT_ERR_PASS) {
        ESP_LOGW(TAG, "TWAI error-passive (no ACK? vehicle not connected)");
    }
    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGE(TAG, "TWAI bus-off — initiating recovery");
        twai_initiate_recovery();     // 需要 128 × 11 個隱性位元才會完成
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        esp_err_t r = twai_start();
        ESP_LOGW(TAG, "TWAI bus recovered — restart: %s", esp_err_to_name(r));
    }
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
