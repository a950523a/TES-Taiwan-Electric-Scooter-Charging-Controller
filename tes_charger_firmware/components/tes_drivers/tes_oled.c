#include "tes_oled.h"
#include "u8g2.h"

// 引用外部定義的回調函數 (在 tes_u8g2_hal.c)
extern uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
extern uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

static u8g2_t u8g2;

void tes_oled_init(void) {
    // 這裡設定具體的螢幕型號
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb
    );
    
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0); // 喚醒
    
    // --- 關鍵：清除雪花屏 ---
    u8g2_ClearBuffer(&u8g2);
    u8g2_SendBuffer(&u8g2);
    
    // 設定預設字型
    u8g2_SetFont(&u8g2, u8g2_font_ncenB08_tr);
}

void tes_oled_clear(void) {
    u8g2_ClearBuffer(&u8g2);
}

void tes_oled_draw_str(int x, int y, const char *str) {
    if (str) {
        u8g2_DrawStr(&u8g2, x, y, str);
    }
}

void tes_oled_update(void) {
    u8g2_SendBuffer(&u8g2);
}