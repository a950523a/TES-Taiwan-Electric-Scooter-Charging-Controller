#include "tes_protocol.h"
#include "tes_can.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TES_PROTO";

// 實體化資料物件
tes_vehicle_data_t tes_vehicle;
tes_charger_data_t tes_charger;

// --- 發送函數 (Tx) ---

static void send_0x508(void) {
    uint8_t d[8];
    d[0] = tes_charger.fault_flags.raw;
    d[1] = tes_charger.status_flags.raw;
    d[2] = tes_charger.output_voltage_100mV & 0xFF;
    d[3] = (tes_charger.output_voltage_100mV >> 8) & 0xFF;
    d[4] = tes_charger.output_current_100mA & 0xFF;
    d[5] = (tes_charger.output_current_100mA >> 8) & 0xFF;
    d[6] = tes_charger.error_voltage_limit_100mV & 0xFF;
    d[7] = (tes_charger.error_voltage_limit_100mV >> 8) & 0xFF;
    tes_can_send(0x508, d, 8);
}

static void send_0x509(void) {
    uint8_t d[8];
    d[0] = tes_charger.sequence_number;
    d[1] = tes_charger.rated_power_50W;
    d[2] = tes_charger.present_voltage_100mV & 0xFF;
    d[3] = (tes_charger.present_voltage_100mV >> 8) & 0xFF;
    d[4] = tes_charger.present_current_100mA & 0xFF;
    d[5] = (tes_charger.present_current_100mA >> 8) & 0xFF;
    d[6] = tes_charger.remaining_time_min & 0xFF;
    d[7] = (tes_charger.remaining_time_min >> 8) & 0xFF;
    tes_can_send(0x509, d, 8);
}

static void send_0x5F8(void) {
    uint8_t d[8] = {0};
    d[0] = tes_charger.emergency_stop_status ? 0x01 : 0x00;
    // Byte 1-3 保留
    d[4] = tes_charger.manufacturer_id & 0xFF;
    d[5] = (tes_charger.manufacturer_id >> 8) & 0xFF;
    // Byte 6-7 保留
    tes_can_send(0x5F8, d, 8);
}

// --- 接收解析函數 (Rx) ---

static void parse_0x500(const uint8_t *d) {
    tes_vehicle.fault_flags.raw = d[0];
    tes_vehicle.status_flags.raw = d[1];
    tes_vehicle.charge_current_request_100mA = d[2] | (d[3] << 8);
    tes_vehicle.charge_voltage_limit_100mV = d[4] | (d[5] << 8);
    tes_vehicle.max_charge_voltage_100mV = d[6] | (d[7] << 8);
}

static void parse_0x501(const uint8_t *d) {
    tes_vehicle.charge_sequence_number = d[0];
    tes_vehicle.soc = d[1];
    tes_vehicle.max_charge_time_min = d[2] | (d[3] << 8);
    tes_vehicle.estimated_end_time_min = d[4] | (d[5] << 8);
}

static void parse_0x5F0(const uint8_t *d) {
    uint8_t flags = d[0];
    tes_vehicle.emergency_stop_request = (flags & 0x01) ? 1 : 0;
    tes_vehicle.welding_detection_error = (flags & 0x02) ? 1 : 0;
    
    // Byte 1 保留
    tes_vehicle.max_current_capacity_100mA = d[2] | (d[3] << 8);
}

// --- 主任務 ---
static void tes_protocol_task(void *arg) {
    uint32_t rx_id;
    uint8_t rx_data[8];
    uint8_t rx_len;
    
    // 初始化預設值
    tes_charger.sequence_number = 0x12; // 版本號 18
    tes_charger.status_flags.bits.stop_control = 1; // 預設停止狀態

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(100); // 100ms

    while (1) {
        // 1. 定期發送
        send_0x508();
        send_0x509();
        send_0x5F8();

        // 2. 接收處理 (非阻塞，一次讀完所有 buffer)
        while (tes_can_receive(&rx_id, rx_data, &rx_len)) {
            switch (rx_id) {
                case 0x500: parse_0x500(rx_data); break;
                case 0x501: parse_0x501(rx_data); break;
                case 0x5F0: parse_0x5F0(rx_data); break;
            }
        }

        vTaskDelayUntil(&last_wake_time, period);
    }
}

void tes_protocol_init(void) {
    memset(&tes_vehicle, 0, sizeof(tes_vehicle));
    memset(&tes_charger, 0, sizeof(tes_charger));
    
    xTaskCreate(tes_protocol_task, "tes_proto", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Protocol Task Started");
}