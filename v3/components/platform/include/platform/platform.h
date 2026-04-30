#pragma once
// platform.h — 平台抽象層（PAL）
// 移植到 STM32 等平台時，只需替換 platform_xxx.c 的實作
#include <stdint.h>

// 返回開機後的單調時間（毫秒），32-bit 約 49 天才溢位
uint32_t platform_tick_ms(void);
