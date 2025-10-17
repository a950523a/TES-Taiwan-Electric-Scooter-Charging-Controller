#ifndef HAL_H
#define HAL_H

#include <Arduino.h>
#include "Charger_Defs.h"

enum ButtonType {
    BUTTON_START,
    BUTTON_STOP,
    BUTTON_EMERGENCY,
    BUTTON_SETTING
};

// 初始化函數
void hal_init_pins();
void hal_init_can();
void hal_init_adc();

// 輸出控制
void hal_update_leds(LedState state); 
void hal_control_charge_relay(bool on);
void hal_control_coupler_lock(bool lock);
void hal_control_vp_relay(bool on);

bool hal_get_charge_relay_state();

// 輸入讀取
bool hal_get_button_state(ButtonType button);
float hal_read_voltage_sensor();
float hal_read_power_supply_voltage();
float hal_read_cp_voltage();
// 注意：hal_read_current_sensor() 已被移除，因為電流是從CAN讀取或由邏輯層模擬，不屬於HAL的職責

// CAN 通訊接口
void hal_can_send(unsigned long id, byte* data, byte len);
bool hal_can_receive(unsigned long* id, byte* len, byte* buf);


#endif // HAL_H