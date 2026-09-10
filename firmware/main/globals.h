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

// 任務清單，供 task_monitor 監看各自的堆疊餘裕。
// 建立時就保存 handle，不依賴 INCLUDE_xTaskGetHandle 之類的 FreeRTOS 設定。
typedef struct {
    const char  *name;
    TaskHandle_t handle;
} task_entry_t;

#define G_TASK_MAX 12
extern task_entry_t g_tasks[G_TASK_MAX];
extern int          g_task_count;

// 會自行 vTaskDelete(NULL) 結束的任務，必須在刪除前呼叫這個把自己的 handle 清掉，
// 否則 task_monitor 會拿懸空的 handle 去讀堆疊資訊而當機。
void g_task_unregister_self(void);

// ADC readings: task_hal_poll -> task_tes_sm (single writer, no mutex needed)
extern volatile float    g_adc_cp_voltage;
extern volatile float    g_adc_output_voltage;
// 每取得一次新的 CP 取樣 +1。TES SM 靠它分辨「新資料」與「同一筆重複讀到」，
// 否則 10ms 的 tick 會把 100ms 才更新一次的取樣重複計入 CP 去抖計數器。
extern volatile uint32_t g_adc_cp_seq;
