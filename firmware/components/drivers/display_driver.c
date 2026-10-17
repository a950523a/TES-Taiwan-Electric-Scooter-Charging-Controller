// display_driver.c — u8g2 SSD1306 128x64 via ESP-IDF I2C (HAL wrapper)
// Managed component: olikraus/u8g2 (declared in idf_component.yml).
// Run `idf.py update-dependencies` once before building.

#include "drivers/display_driver.h"
#include "hal/hal_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"
#include <string.h>

#define OLED_I2C_ADDR   0x3C   // 7-bit; match V2 config (0x3C or 0x3D)
#define I2C_TX_BUF_MAX  256    // max bytes per u8g2 I2C transaction (1 page = ~130 bytes)

static const char *TAG = "display_driver";

static u8g2_t  s_u8g2;
static bool    s_ok = false;

// 整個畫面的位移（待機防烙印：每隔一段時間偏 1 px）。所有繪圖函式都加上它
static int     s_ox, s_oy;
#define X(x) ((u8g2_uint_t)((x) + s_ox))
#define Y(y) ((u8g2_uint_t)((y) + s_oy))

// 中文字型（只含韌體用到的字，見 tools/gen_oled_font.py）
extern const uint8_t u8g2_font_tes_zh12[];

// u8g2 byte-level I2C callback — buffer bytes between START and END TRANSFER
static uint8_t s_tx_buf[I2C_TX_BUF_MAX];
static int     s_tx_len;

static uint8_t u8x8_byte_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        break;
    case U8X8_MSG_BYTE_START_TRANSFER:
        s_tx_len = 0;
        break;
    case U8X8_MSG_BYTE_SEND: {
        const uint8_t *p = (const uint8_t *)arg_ptr;
        for (uint8_t i = 0; i < arg_int; i++) {
            if (s_tx_len < I2C_TX_BUF_MAX)
                s_tx_buf[s_tx_len++] = p[i];
        }
        break;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        hal_i2c_write(OLED_I2C_ADDR, s_tx_buf, (size_t)s_tx_len);
        break;
    default:
        return 0;
    }
    return 1;
}

// u8g2 GPIO/delay callback — no hardware GPIO needed for I2C OLED
static uint8_t u8x8_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int ? arg_int : 1));
        break;
    case U8X8_MSG_DELAY_NANO:
    case U8X8_MSG_GPIO_CS:
    case U8X8_MSG_GPIO_DC:
    case U8X8_MSG_GPIO_RESET:
        break;
    default:
        return 0;
    }
    return 1;
}

esp_err_t display_driver_init(void)
{
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2, U8G2_R2, u8x8_byte_i2c, u8x8_gpio_and_delay);

    // Probe: attempt I2C write before spending time on full init
    const uint8_t probe = 0x00;
    if (hal_i2c_write(OLED_I2C_ADDR, &probe, 1) != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not found at 0x%02X", OLED_I2C_ADDR);
        return ESP_OK;   // non-fatal — display_driver_is_ok() returns false
    }

    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);   // display on
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
    s_ok = true;
    ESP_LOGI(TAG, "SSD1306 128x64 ready at I2C 0x%02X", OLED_I2C_ADDR);
    return ESP_OK;
}

bool     display_driver_is_ok   (void)  { return s_ok; }
void     display_driver_clear   (void)  { u8g2_ClearBuffer(&s_u8g2); }
void     display_driver_flush   (void)  { u8g2_SendBuffer(&s_u8g2); }

void display_driver_font_small (void) { u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tr); }
void display_driver_font_medium(void) { u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr); }
void display_driver_font_large (void) { u8g2_SetFont(&s_u8g2, u8g2_font_8x13_tr); }
void display_driver_font_bold  (void) { u8g2_SetFont(&s_u8g2, u8g2_font_7x13B_tr); }
// 大號數字（約 20 px 高），只有數字與少數符號 —— 配對碼用
void display_driver_font_digits(void) { u8g2_SetFont(&s_u8g2, u8g2_font_logisoso20_tn); }
// 12 px 中文（efont b12，含半形 ASCII 6 px）。字串要用 display_driver_draw_utf8 畫
void display_driver_font_zh    (void) { u8g2_SetFont(&s_u8g2, u8g2_font_tes_zh12); }

void display_driver_set_offset(int dx, int dy) { s_ox = dx; s_oy = dy; }

void display_driver_draw_utf8(int x, int y, const char *s)
{
    u8g2_DrawUTF8(&s_u8g2, X(x), Y(y), s);
}

uint16_t display_driver_utf8_width(const char *s)
{
    return (uint16_t)u8g2_GetUTF8Width(&s_u8g2, s);
}

void display_driver_set_color(uint8_t color)
{
    u8g2_SetDrawColor(&s_u8g2, color);
}

void display_driver_draw_str(int x, int y, const char *s)
{
    u8g2_DrawStr(&s_u8g2, X(x), Y(y), s);
}

void display_driver_draw_hline(int x, int y, int w)
{
    u8g2_DrawHLine(&s_u8g2, X(x), Y(y), (u8g2_uint_t)w);
}

void display_driver_draw_box(int x, int y, int w, int h)
{
    u8g2_DrawBox(&s_u8g2, X(x), Y(y), (u8g2_uint_t)w, (u8g2_uint_t)h);
}

void display_driver_draw_frame(int x, int y, int w, int h)
{
    u8g2_DrawFrame(&s_u8g2, X(x), Y(y), (u8g2_uint_t)w, (u8g2_uint_t)h);
}

uint16_t display_driver_str_width(const char *s)
{
    return (uint16_t)u8g2_GetStrWidth(&s_u8g2, s);
}

void display_driver_draw_circle(int x, int y, int r)
{
    u8g2_DrawCircle(&s_u8g2, X(x), Y(y), (u8g2_uint_t)r, U8G2_DRAW_ALL);
}

void display_driver_draw_disc(int x, int y, int r)
{
    u8g2_DrawDisc(&s_u8g2, X(x), Y(y), (u8g2_uint_t)r, U8G2_DRAW_ALL);
}

void display_driver_draw_line(int x1, int y1, int x2, int y2)
{
    u8g2_DrawLine(&s_u8g2, X(x1), Y(y1), X(x2), Y(y2));
}

void display_driver_draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2)
{
    u8g2_DrawTriangle(&s_u8g2, x0 + s_ox, y0 + s_oy, x1 + s_ox, y1 + s_oy, x2 + s_ox, y2 + s_oy);
}

void display_driver_draw_glyph(int x, int y, uint16_t g)
{
    u8g2_DrawGlyph(&s_u8g2, X(x), Y(y), g);
}
