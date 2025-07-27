// src/CAN_Protocol/CAN_Protocol.h

#ifndef CAN_PROTOCOL_H
#define CAN_PROTOCOL_H

#include "Charger_Defs.h" // 引入公共定義
#include "Config.h"       // 引入CAN ID等配置

// --- 聲明全局的、已解析的數據存儲變數 ---
// ChargerLogic 將從這裡讀取車輛的最新狀態
extern CAN_Vehicle_Status_500 vehicleStatus500;
extern CAN_Vehicle_Params_501 vehicleParams501;
extern CAN_Vehicle_Emergency_5F0 vehicleEmergency5F0;

// --- 公開的API函數 ---
void can_protocol_handle_receive(); // 在主循環中調用，處理接收

// --- 發送函數 ---
void can_protocol_send_charger_status(const CAN_Charger_Status_508& status);
void can_protocol_send_charger_params(const CAN_Charger_Params_509& params);
void can_protocol_send_emergency_stop(const CAN_Charger_Emergency_5F8& emergency);

#endif // CAN_PROTOCOL_H