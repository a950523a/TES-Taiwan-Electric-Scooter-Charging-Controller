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
void display_driver_font_digits (void);  // 大號數字（logisoso20，只有數字）
void display_driver_font_zh     (void);  // 12 px 中文＋半形 ASCII（只含韌體用到的字）

// OLED 上的中文字串一律寫成 ZH("...")：它就是字串本身，但 tools/gen_oled_font.py 靠它
// 找出要放進字型的字。直接寫 "中文" 不會報錯，只會在 OLED 上畫不出來
#define ZH(s) s

// 整個畫面位移 dx/dy px（待機防烙印）。之後的所有繪圖都會加上，畫完記得設回 0
void     display_driver_set_offset  (int dx, int dy);
// UTF-8 字串（中文）。英文字型也能用，但中文只有 font_zh 有
void     display_driver_draw_utf8   (int x, int y, const char *str);
uint16_t display_driver_utf8_width  (const char *str);

// 繪圖
void     display_driver_set_color  (uint8_t color);            // 0=clear 1=set 2=XOR
void     display_driver_draw_str   (int x, int y, const char *str);
void     display_driver_draw_hline   (int x, int y, int w);
void     display_driver_draw_box     (int x, int y, int w, int h);
void     display_driver_draw_frame   (int x, int y, int w, int h);
void     display_driver_draw_circle  (int x, int y, int r);
void     display_driver_draw_disc    (int x, int y, int r);
void     display_driver_draw_line    (int x1, int y1, int x2, int y2);
void     display_driver_draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2);
uint16_t display_driver_str_width    (const char *str);
void     display_driver_draw_glyph   (int x, int y, uint16_t glyph);
