#pragma once
#include "tes_protocol/tes_types.h"
#include <stdbool.h>
#include <stdint.h>

// OLED status / settings-menu service.
// Runs in task_display (50ms tick).
// Reads tes_snapshot_t; never touches the state machine directly.

typedef enum {
    DISP_SCREEN_STATUS = 0,   // main charging status screen
    DISP_SCREEN_MENU,         // settings menu
    DISP_SCREEN_FAULT,        // fault detail
} disp_screen_t;

void display_svc_init  (void);
void display_svc_tick  (void);             // call every 50ms
void display_svc_button(uint8_t btn_evt);  // EVT_BUTTON_START / STOP / SETTING
