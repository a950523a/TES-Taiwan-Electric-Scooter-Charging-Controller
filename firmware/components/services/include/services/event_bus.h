#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Event bus: replaces the V2 dual-mutex + shared DisplayData pattern.
// Publishers call event_bus_publish(); subscribers receive via their queue handle.
// Max 4 subscribers. Payload is fixed-size to avoid dynamic allocation.

typedef enum {
    EVT_TES_STATE_CHANGED = 0,
    EVT_TES_SOC_UPDATED,
    EVT_TES_FAULT,
    EVT_TES_CHARGE_COMPLETE,
    EVT_BUTTON_START,
    EVT_BUTTON_STOP,
    EVT_BUTTON_EMERGENCY,
    EVT_BUTTON_SETTING,
    EVT_BUTTON_START_LONG,
    EVT_BUTTON_STOP_LONG,
    EVT_BUTTON_SETTING_LONG,
    EVT_WIFI_CHANGED,
    EVT_OTA_PROGRESS,
    EVT_OTA_COMPLETE,
    EVT_FAULT_CLEAR,
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     timestamp_ms;
    uint8_t      payload[24];
} charger_event_t;

void          event_bus_init     (void);
void          event_bus_publish  (const charger_event_t *evt);
QueueHandle_t event_bus_subscribe(void);  // caller owns the returned queue
