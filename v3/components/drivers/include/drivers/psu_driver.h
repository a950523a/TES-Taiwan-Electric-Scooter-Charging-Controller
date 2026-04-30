#pragma once
#include <esp_err.h>
#include <stdbool.h>

// PSU UART 文字協議（與 V2 相同格式）
// RX:  "V=xx.x,I=xx.x\n"
// TX:  "SET:V=xx.x\n" / "SET:I=xx.x\n"
// 注意：V3 完全去掉 Arduino String，改用 char[] + snprintf

typedef struct {
    float voltage;   // V
    float current;   // A
    bool  connected;
} psu_status_t;

esp_err_t    psu_driver_init     (void);
void         psu_driver_poll     (void);              // 非阻塞，由 task_hal_poll 每 tick 呼叫
void         psu_driver_set_voltage(float v);
void         psu_driver_set_current(float a);
psu_status_t psu_driver_get_status(void);
