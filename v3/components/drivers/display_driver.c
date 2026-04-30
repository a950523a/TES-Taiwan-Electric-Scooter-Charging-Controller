#include "drivers/display_driver.h"
#include "hal/hal_i2c.h"
#include "esp_log.h"
#include "u8g2.h"
#include <string.h>

static const char *TAG = "display_driver";

static u8g2_t s_u8g2;
static bool   s_ok = false;

// ESP-IDF I2C callback for u8g2
static uint8_t u8g2_esp32_i2c_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_BYTE_SEND:
        hal_i2c_write(u8x8_GetI2CAddress(u8x8) >> 1, (uint8_t *)arg_ptr, arg_int);
        break;
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_SET_DC:
    case U8X8_MSG_BYTE_START_TRANSFER:
    case U8X8_MSG_BYTE_END_TRANSFER:
        break;
    default:
        return 0;
    }
    return 1;
}

static uint8_t u8g2_esp32_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)arg_ptr;
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    default:
        return 0;
    }
    return 1;
}

esp_err_t display_driver_init(void)
{
    // Try 0x3C first, fall back to 0x3D
    static const uint8_t addrs[] = { 0x3C, 0x3D };
    for (int i = 0; i < 2; i++) {
        u8g2_Setup_ssd1306_i2c_128x64_noname_f(
            &s_u8g2, U8G2_R0,
            u8g2_esp32_i2c_cb,
            u8g2_esp32_gpio_delay_cb);
        u8x8_SetI2CAddress(&s_u8g2.u8x8, addrs[i] << 1);
        u8g2_InitDisplay(&s_u8g2);
        u8g2_SetPowerSave(&s_u8g2, 0);
        s_ok = true;
        ESP_LOGI(TAG, "SSD1306 at 0x%02X", addrs[i]);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "SSD1306 not found");
    return ESP_FAIL;
}

bool display_driver_is_ok(void) { return s_ok; }

void display_driver_clear(void)  { u8g2_ClearBuffer(&s_u8g2); }
void display_driver_flush(void)  { u8g2_SendBuffer(&s_u8g2); }

void display_driver_font_small(void)  { u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tr); }
void display_driver_font_medium(void) { u8g2_SetFont(&s_u8g2, u8g2_font_8x13_tr); }
void display_driver_font_large(void)  { u8g2_SetFont(&s_u8g2, u8g2_font_10x20_tr); }
void display_driver_font_bold(void)   { u8g2_SetFont(&s_u8g2, u8g2_font_7x13B_tr); }

void display_driver_draw_str(int x, int y, const char *str)
{
    u8g2_DrawStr(&s_u8g2, x, y, str);
}

void display_driver_draw_hline(int x, int y, int w)
{
    u8g2_DrawHLine(&s_u8g2, x, y, w);
}

void display_driver_draw_box(int x, int y, int w, int h)
{
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
}

void display_driver_draw_frame(int x, int y, int w, int h)
{
    u8g2_DrawFrame(&s_u8g2, x, y, w, h);
}

uint16_t display_driver_str_width(const char *str)
{
    return u8g2_GetStrWidth(&s_u8g2, str);
}

void display_driver_draw_glyph(int x, int y, uint16_t glyph)
{
    u8g2_DrawGlyph(&s_u8g2, x, y, glyph);
}
