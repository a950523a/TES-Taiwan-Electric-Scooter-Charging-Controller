#pragma once
#include "tes_protocol/tes_types.h"
#include <stdbool.h>

// LED 驅動（整合 V2 LuxBeacon 狀態機）
// 橘燈（STANDBY）永遠常亮
// 綠燈（CHARGING）：待機=OFF，充電中=閃爍或 LuxBeacon，完成=常亮
// 紅燈（ERROR）：故障時常亮

void led_driver_init             (void);
void led_driver_set_state        (led_state_t state);
void led_driver_set_beacon_enable(bool enabled);     // beacon_unlocked 旗標
void led_driver_set_beacon_soc   (int soc);          // 更新 LuxBeacon 數值
void led_driver_tick             (void);             // 每 50ms 由 task_display 呼叫
