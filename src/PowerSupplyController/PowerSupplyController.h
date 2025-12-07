#ifndef POWER_SUPPLY_CONTROLLER_H
#define POWER_SUPPLY_CONTROLLER_H

#include <Arduino.h>

void psc_init();
void psc_handle_task();

// 發送指令
void psc_set_voltage(float v);
void psc_set_current(float a);

// 獲取狀態
bool psc_is_connected();
float psc_get_voltage();
float psc_get_current();

#endif