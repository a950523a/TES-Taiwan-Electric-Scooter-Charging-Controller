#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// u8g2 驅動包裝（ESP-IDF I2C callback 模式）
// OLED SSD1306 128x64，I2C 位址 0x3C 或 0x3D（自動偵測）

esp_err_t display_driver_init    (void);
bool      display_driver_is_ok   (void);  // 是否成功初始化
void      display_driver_clear   (void);
void      display_driver_flush   (void);  // 送出 buffer

// 字型
void display_driver_font_small  (void);  // 5x8 or 6x10
void display_driver_font_medium (void);  // 8pt
void display_driver_font_large  (void);  // 10-12pt
void display_driver_font_bold   (void);  // 7x13B

// 繪圖
void     display_driver_set_color  (uint8_t color);            // 0=clear 1=set 2=XOR
void     display_driver_draw_str   (int x, int y, const char *str);
void     display_driver_draw_hline (int x, int y, int w);
void     display_driver_draw_box   (int x, int y, int w, int h);
void     display_driver_draw_frame (int x, int y, int w, int h);
uint16_t display_driver_str_width  (const char *str);
void     display_driver_draw_glyph (int x, int y, uint16_t glyph);
