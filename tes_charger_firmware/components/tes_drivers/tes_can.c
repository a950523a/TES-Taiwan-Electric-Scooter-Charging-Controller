#include "tes_can.h"
#include "driver/twai.h"
#include "board_bsp.h" // 為了拿 BOARD_CAN_PINS
#include "esp_log.h"   // 取代 Serial.print

static const char *TAG = "TES_CAN";

esp_err_t tes_can_init(void) {
    // 1. 讀取 BSP 定義的腳位
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)BOARD_CAN_PINS.tx_io, 
        (gpio_num_t)BOARD_CAN_PINS.rx_io, 
        TWAI_MODE_NORMAL
    );
    
    g_config.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS;

    // 2. 設定速率 500Kbps (TES 標準)
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    
    // 3. 過濾器 (先接收所有)
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // 4. 安裝驅動
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "Driver installed");
    } else {
        ESP_LOGE(TAG, "Failed to install driver");
        return ESP_FAIL;
    }

    // 5. 啟動驅動
    if (twai_start() == ESP_OK) {
        ESP_LOGI(TAG, "Driver started");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to start driver");
        return ESP_FAIL;
    }
}

void tes_can_check_status(void) {
    uint32_t alerts;
    // 讀取警報 (非阻塞)
    if (twai_read_alerts(&alerts, 0) == ESP_OK) {
        
        if (alerts & TWAI_ALERT_TX_FAILED) {
            // 發送失敗 (通常是因為沒人 ACK)
            // 這是正常現象，不需要 Panic，紀錄一下就好
            ESP_LOGW(TAG, "Alert: TX Failed (No ACK?)");
        }

        if (alerts & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "Alert: Bus-Off condition! Initiating recovery...");
            // 發生 Bus-Off，必須手動發起恢復
            twai_initiate_recovery();
        }

        if (alerts & TWAI_ALERT_BUS_RECOVERED) {
            ESP_LOGI(TAG, "Alert: Bus Recovered! Restarting driver...");
            // 恢復完成後，必須重新 start
            twai_start();
        }
    }
}

esp_err_t tes_can_send(uint32_t id, const uint8_t *data, uint8_t len) {
    twai_status_info_t status_info;
    twai_get_status_info(&status_info);
    
    if (status_info.state != TWAI_STATE_RUNNING) {
        // 如果正在恢復中，或是 Bus-Off，直接返回錯誤，不要卡住
        return ESP_ERR_INVALID_STATE;
    }
    
    twai_message_t message;
    message.identifier = id;
    message.extd = 0;             // 標準幀 (Standard Frame)
    message.rtr = 0;              // 數據幀
    message.data_length_code = len;
    
    for (int i = 0; i < len; i++) {
        message.data[i] = data[i];
    }

    return twai_transmit(&message, pdMS_TO_TICKS(10));
}

bool tes_can_receive(uint32_t *id, uint8_t *data, uint8_t *len) {
    twai_message_t message;
    
    // pdMS_TO_TICKS(0) 代表不等待 (Non-blocking)
    if (twai_receive(&message, pdMS_TO_TICKS(0)) == ESP_OK) {
        *id = message.identifier;
        *len = message.data_length_code;
        
        for (int i = 0; i < message.data_length_code; i++) {
            data[i] = message.data[i];
        }
        return true;
    }
    return false;
}