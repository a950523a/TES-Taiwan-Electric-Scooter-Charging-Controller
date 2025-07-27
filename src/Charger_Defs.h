// src/Charger_Defs.h

#ifndef CHARGER_DEFS_H
#define CHARGER_DEFS_H

#include <Arduino.h> // 為了 byte 類型

// --- LED 狀態枚舉 ---
enum LedState {
    LED_STATE_STANDBY,
    LED_STATE_CHARGING,
    LED_STATE_COMPLETE,
    LED_STATE_FAULT
};

// --- CP 狀態定義 ---
enum CPState : byte {
    CP_STATE_UNKNOWN,
    CP_STATE_OFF,
    CP_STATE_ON,
    CP_STATE_ERROR
};

// --- 充電狀態機狀態定義 ---
enum ChargerState : byte {
    STATE_CHG_IDLE,
    STATE_CHG_INITIAL_PARAM_EXCHANGE,
    STATE_CHG_PRE_CHARGE_OPERATIONS,
    STATE_CHG_DC_CURRENT_OUTPUT,
    STATE_CHG_ENDING_CHARGE_PROCESS,
    STATE_CHG_FAULT_HANDLING,
    STATE_CHG_EMERGENCY_STOP_PROC,
    STATE_CHG_FINALIZATION
};

// --- UI 狀態枚舉 ---
enum UIState {
    UI_STATE_NORMAL,
    UI_STATE_MENU_MAIN,
    UI_STATE_MENU_SET_VOLTAGE,
    UI_STATE_MENU_SET_CURRENT,
    UI_STATE_MENU_SET_SOC,
    UI_STATE_MENU_SAVED
};

// --- CAN訊息資料結構 ---
// 這些結構體現在是公共的，因為 Logic層需要填充它們，CAN層需要解析/打包它們
struct CAN_Charger_Status_508 {
    byte faultFlags;
    byte statusFlags;
    unsigned int availableVoltage;
    unsigned int availableCurrent;
    unsigned int faultDetectionVoltageLimit;
};
struct CAN_Charger_Params_509 {
    byte esChargeSequenceNumber;
    byte ratedOutputPower;
    unsigned int actualOutputVoltage;
    unsigned int actualOutputCurrent;
    unsigned int remainingChargeTime;
};
struct CAN_Charger_Emergency_5F8 {
    byte emergencyStopRequestFlags;
    byte reserved1_3[3];
    unsigned int chargerManufacturerID;
};
struct CAN_Vehicle_Status_500 {
    byte faultFlags;
    byte statusFlags;
    unsigned int chargeCurrentCommand;
    unsigned int chargeVoltageLimit;
    unsigned int maxChargeVoltage;
};
struct CAN_Vehicle_Params_501 {
    byte esChargeSequenceNumber;
    byte stateOfCharge;
    unsigned int maxChargeTime;
    unsigned int estimatedChargeEndTime;
    byte reserved[2];
};
struct CAN_Vehicle_Emergency_5F0 {
    byte errorRequestFlags;
};


#endif // CHARGER_DEFS_H