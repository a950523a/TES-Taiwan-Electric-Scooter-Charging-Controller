#include "CAN_Protocol.h"
#include "HAL/HAL.h" // 翻譯部門需要和收發室(HAL)打交道

// --- 定義全局數據存儲變數的實體 ---
CAN_Vehicle_Status_500 vehicleStatus500;
CAN_Vehicle_Params_501 vehicleParams501;
CAN_Vehicle_Emergency_5F0 vehicleEmergency5F0;


void can_protocol_handle_receive() {
    unsigned long id;
    byte len;
    byte buf[8];
    // 從收發室(HAL)獲取原始CAN報文
    while (hal_can_receive(&id, &len, buf)) {
    // 開始翻譯（解析）
        if (xSemaphoreTake(canDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            switch (id) {
                case VEHICLE_STATUS_ID:
                    if (len == 8) {
                        vehicleStatus500.faultFlags = buf[0];
                        vehicleStatus500.statusFlags = buf[1];
                        vehicleStatus500.chargeCurrentCommand = (unsigned int)(buf[3] << 8 | buf[2]);
                        vehicleStatus500.chargeVoltageLimit = (unsigned int)(buf[5] << 8 | buf[4]);
                        vehicleStatus500.maxChargeVoltage = (unsigned int)(buf[7] << 8 | buf[6]);
                    }
                    break;
                    
                case VEHICLE_PARAMS_ID:
                    if (len >= 4) {
                         vehicleParams501.esChargeSequenceNumber = buf[0];
                        vehicleParams501.stateOfCharge = buf[1];
                        vehicleParams501.maxChargeTime = (uint16_t)(buf[3] << 8 | buf[2]);
                        if (len >= 6) {
                            vehicleParams501.estimatedChargeEndTime = (uint16_t)(buf[5] << 8 | buf[4]);
                        }
                    }
                    break;

                case VEHICLE_EMERGENCY_ID:
                    if (len >= 1) {
                        vehicleEmergency5F0.errorRequestFlags = buf[0];
                    }
                    break;
            }
            xSemaphoreGive(canDataMutex);
        }
    }
}


void can_protocol_send_charger_status(const CAN_Charger_Status_508& status) {
    byte data[8];
    data[0] = status.faultFlags;
    data[1] = status.statusFlags;
    data[2] = status.availableVoltage & 0xFF;
    data[3] = (status.availableVoltage >> 8) & 0xFF;
    data[4] = status.availableCurrent & 0xFF;
    data[5] = (status.availableCurrent >> 8) & 0xFF;
    data[6] = status.faultDetectionVoltageLimit & 0xFF;
    data[7] = (status.faultDetectionVoltageLimit >> 8) & 0xFF;
    hal_can_send(CHARGER_STATUS_ID, data, 8);
}

void can_protocol_send_charger_params(const CAN_Charger_Params_509& params) {
    byte data[8];
    data[0] = params.esChargeSequenceNumber;
    data[1] = params.ratedOutputPower;
    data[2] = params.actualOutputVoltage & 0xFF;
    data[3] = (params.actualOutputVoltage >> 8) & 0xFF;
    data[4] = params.actualOutputCurrent & 0xFF;
    data[5] = (params.actualOutputCurrent >> 8) & 0xFF;
    data[6] = params.remainingChargeTime & 0xFF;
    data[7] = (params.remainingChargeTime >> 8) & 0xFF;
    hal_can_send(CHARGER_PARAMS_ID, data, 8);
}

void can_protocol_send_emergency_stop(const CAN_Charger_Emergency_5F8& emergency) {
    byte d[8] = {0};
    d[0] = emergency.emergencyStopRequestFlags;
    d[4] = emergency.chargerManufacturerID & 0xFF;
    d[5] = (emergency.chargerManufacturerID >> 8) & 0xFF;
    hal_can_send(CHARGER_EMERGENCY_STOP_ID, d, 8);
}

CAN_Vehicle_Status_500 can_protocol_get_vehicle_status() {
    CAN_Vehicle_Status_500 status_snapshot;
    
    // 使用Mutex來保證線程安全地複製數據
    if (xSemaphoreTake(canDataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(&status_snapshot, &vehicleStatus500, sizeof(CAN_Vehicle_Status_500));
        xSemaphoreGive(canDataMutex);
    } else {
        // 如果獲取鎖失敗，返回一個清零的結構體，防止上層使用髒數據
        memset(&status_snapshot, 0, sizeof(CAN_Vehicle_Status_500));
        Serial.println("WARN: Failed to get mutex in can_protocol_get_vehicle_status!");
    }
    
    return status_snapshot;
}