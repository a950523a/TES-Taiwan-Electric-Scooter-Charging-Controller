#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 OLED
void tes_oled_init(void);

// 清除畫面
void tes_oled_clear(void);

// 繪製文字
void tes_oled_draw_str(int x, int y, const char *str);

// 刷新畫面 (將 Buffer 送出)
void tes_oled_update(void);

void tes_oled_update_emergency(void);

#ifdef __cplusplus
}
#endif