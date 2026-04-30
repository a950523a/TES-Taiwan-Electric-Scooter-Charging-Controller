#pragma once
#include "tes_protocol/tes_sm.h"
#include "tes_protocol/tes_types.h"
#include "drivers/can_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

// CAN RX → TES SM: raw frames
extern QueueHandle_t g_can_rx_queue;

// TES SM → display / network: read-only snapshot
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;

// Emergency stop: set by task_hal_poll (EMERGENCY button), read by task_tes_sm every tick
extern atomic_bool g_emergency_stop;

// Button events: task_hal_poll → task_tes_sm
extern QueueHandle_t g_btn_event_queue;  // element type: uint8_t (EVT_BUTTON_*)

// ADC readings: task_hal_poll → task_tes_sm (no mutex needed, single writer)
extern volatile float g_adc_cp_voltage;       // CP signal voltage (V)
extern volatile float g_adc_output_voltage;   // output side voltage (V)
