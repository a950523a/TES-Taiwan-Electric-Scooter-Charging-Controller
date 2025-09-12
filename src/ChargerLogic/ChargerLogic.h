// src/ChargerLogic/ChargerLogic.h

#ifndef CHARGERLOGIC_H
#define CHARGERLOGIC_H

#include "Charger_Defs.h" 

// --- 公開 API 函數 ---
void logic_init();
void logic_run_statemachine();
void logic_handle_periodic_tasks();
void logic_save_config(unsigned int voltage, unsigned int current, int soc);
void logic_start_button_pressed();
void logic_stop_button_pressed();
void logic_save_web_settings(unsigned int current, int soc);

// --- [新增] 提供給所有前端的統一數據接口 ---
void logic_get_display_data(DisplayData& data);

// --- 舊的 Getters 現在主要供內部或特定功能使用 ---
LedState logic_get_led_state();
ChargerState logic_get_charger_state();
bool logic_is_fault_latched();
bool logic_is_charge_complete();
int logic_get_soc();
uint32_t logic_get_remaining_seconds();
float logic_get_measured_voltage();
float logic_get_measured_current();
bool logic_is_timer_running();
uint32_t logic_get_total_time_seconds();
unsigned int logic_get_max_voltage_setting();
unsigned int logic_get_max_current_setting();
int logic_get_target_soc_setting();

#endif // CHARGERLOGIC_H