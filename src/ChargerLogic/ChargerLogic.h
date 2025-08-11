#ifndef CHARGERLOGIC_H
#define CHARGERLOGIC_H

#include "Charger_Defs.h" 

// --- 公開 API 函數 ---
void logic_init();
void logic_run_statemachine();
void logic_handle_periodic_tasks();
void logic_save_config(unsigned int voltage, unsigned int current, int soc);\
void logic_start_button_pressed();
void logic_stop_button_pressed();

// --- 提供給其他層讀取狀態的 Getters ---
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