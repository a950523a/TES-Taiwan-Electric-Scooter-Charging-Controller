#pragma once
#include "tes_protocol/tes_sm.h"
#include "tes_protocol/tes_types.h"
#include "drivers/can_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

// CAN RX -> TES SM: raw frames
extern QueueHandle_t g_can_rx_queue;

// TES SM -> display / network: read-only snapshot
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;

// Emergency stop: set by task_hal_poll (EMERGENCY button), read by task_tes_sm every tick
extern atomic_bool g_emergency_stop;

// TES SM button events: task_hal_poll -> task_tes_sm
// Carries EVT_BUTTON_START / EVT_BUTTON_STOP (uint8_t)
extern QueueHandle_t g_btn_event_queue;

// Display button events: task_hal_poll -> task_display
// Carries EVT_BUTTON_START / EVT_BUTTON_STOP / EVT_BUTTON_SETTING (uint8_t)
extern QueueHandle_t g_display_btn_queue;

// Set true by display_svc when menu is open; task_hal_poll gates TES SM button routing
extern volatile bool g_menu_open;

// ADC readings: task_hal_poll -> task_tes_sm (single writer, no mutex needed)
extern volatile float g_adc_cp_voltage;
extern volatile float g_adc_output_voltage;
