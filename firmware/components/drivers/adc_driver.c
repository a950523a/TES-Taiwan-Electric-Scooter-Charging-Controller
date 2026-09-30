#include "drivers/adc_driver.h"
#include "hal/hal_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

static const char *TAG = "adc_driver";

// ADS1115 register addresses
#define ADS_ADDR          0x48
#define ADS_REG_CONV      0x00
#define ADS_REG_CFG       0x01

// Config word fields (written MSB first)
// OS=1 (start single), MUX, PGA=001 (±4.096V), MODE=1 (single-shot), DR=100 (128SPS), COMP_*=default
#define ADS_CFG_OS        (1u << 15)
#define ADS_CFG_MUX_01    (0b000u << 12)  // differential AIN0-AIN1
// CP 從 AIN2−AIN3 差動改成 AIN2 單端：V1.3 把 AIN3 拿去當硬體版本 ID。
// 舊板的 AIN3 焊死接地，兩種讀法結果一樣；而且 AIN3 接的是 U5 旁的本地地、
// 不是 CP 分壓那端的地，差動從來沒有開爾文接法的效果。
#define ADS_CFG_MUX_2G    (0b110u << 12)  // single-ended AIN2
#define ADS_CFG_MUX_3G    (0b111u << 12)  // single-ended AIN3（硬體版本 ID）
#define ADS_CFG_PGA_4V    (0b001u << 9)   // ±4.096V → LSB = 125µV
#define ADS_CFG_MODE_SINGLE (1u << 8)
#define ADS_CFG_DR_128    (0b100u << 5)
#define ADS_CFG_COMP_DIS  (0b11u << 0)

#define ADS_FSR_V         4.096f
#define ADS_COUNTS        32768.0f

static esp_err_t ads_write_config(uint16_t cfg)
{
    uint8_t buf[3] = { ADS_REG_CFG, (uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF) };
    return hal_i2c_write(ADS_ADDR, buf, 3);
}

static bool ads_read_conversion(int16_t *out)
{
    uint8_t reg = ADS_REG_CONV;
    uint8_t raw[2];
    if (hal_i2c_write_read(ADS_ADDR, &reg, 1, raw, 2) != ESP_OK) {
        return false;
    }
    *out = (int16_t)((raw[0] << 8) | raw[1]);
    return true;
}

// 回傳 false 表示這次量測不可信 —— 呼叫端必須沿用上一次的有效值，
// 絕對不能當成 0V：CP 讀到 0V 是合法的 CP_STATE_OFF，會讓狀態機
// 誤判為插頭拔除而中斷充電（單一次 I2C glitch 就足以觸發）。
static bool ads_read(uint16_t mux_bits, float *out)
{
    uint16_t cfg = ADS_CFG_OS | mux_bits | ADS_CFG_PGA_4V |
                   ADS_CFG_MODE_SINGLE | ADS_CFG_DR_128 | ADS_CFG_COMP_DIS;
    if (ads_write_config(cfg) != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(9));  // 128SPS → ~8ms per sample

    int16_t raw;
    if (!ads_read_conversion(&raw)) return false;

    *out = (float)raw * (ADS_FSR_V / ADS_COUNTS);
    return true;
}

// ── 硬體版本辨識 ────────────────────────────────────────────────────────────
// HW_ID = 3.3 V × R37 / (R36 + R37)，每級 3.3 V / 12 = 0.275 V。兩顆 1% 電阻加
// 3.3 V 穩壓 ±2% 的最壞誤差是 ±0.09 V，都落在自己那一級 ±0.1375 V 的範圍內。
#define HW_ID_STEP_V   (3.3f / 12.0f)
#define HW_ID_TOL_V    0.12f
#define HW_ID_SAMPLES  8

typedef struct {
    int8_t      level;
    const char *name;
    float       r1_kohm;     // 分壓上臂
    float       r2_kohm;     // 分壓下臂
} board_rev_t;

// 只列「韌體需要知道」的版本。新增硬體版本：挑一級、在這裡加一行。
static const board_rev_t BOARD_REVS[] = {
    { 0, "V1.1/V1.2", 348.0f, 12.0f  },   // AIN3 焊死接地 —— 現場每一台都是這個
    { 6, "V1.3",      345.0f,  9.09f },   // R36 = R37 = 10k；上臂 115k × 3、下臂 9.09k
};

static float ratio_of(const board_rev_t *r)
{
    return (r->r1_kohm + r->r2_kohm) / r->r2_kohm;
}

// 預設是舊板 —— 讀不到、ADS1115 不在、或級數不認得，一律用這個。
// 30.000 是現場每一台的實際值，猜錯成 V1.3 會把電壓讀高 30%。
static adc_board_t s_board = {
    .level = 0, .id_volts = 0.0f, .name = "V1.1/V1.2", .known = true,
    .volt_ratio = (348.0f + 12.0f) / 12.0f,
};

static int8_t classify_hw_id(float v)
{
    if (v < 0.5f * HW_ID_STEP_V)  return 0;
    if (v > 11.5f * HW_ID_STEP_V) return ADC_HW_ID_LEVEL_EXT;
    int k = (int)lroundf(v / HW_ID_STEP_V);
    if (fabsf(v - (float)k * HW_ID_STEP_V) > HW_ID_TOL_V) return -1;
    return (int8_t)k;
}

static void detect_board(void)
{
    float sum = 0.0f;
    int   n   = 0;
    for (int i = 0; i < HW_ID_SAMPLES; i++) {
        float v;
        if (ads_read(ADS_CFG_MUX_3G, &v)) { sum += v; n++; }
    }
    if (n < HW_ID_SAMPLES / 2) {
        s_board.level = -1;
        s_board.name  = "unknown";
        s_board.known = false;
        ESP_LOGW(TAG, "board ID: AIN3 unreadable (%d/%d samples) — using V1.1/V1.2 divider",
                 n, HW_ID_SAMPLES);
        return;
    }
    float v = sum / (float)n;
    int8_t level = classify_hw_id(v);
    s_board.id_volts = v;
    s_board.level    = level;
    for (size_t i = 0; i < sizeof(BOARD_REVS) / sizeof(BOARD_REVS[0]); i++) {
        if (BOARD_REVS[i].level == level) {
            s_board.name       = BOARD_REVS[i].name;
            s_board.known      = true;
            s_board.volt_ratio = ratio_of(&BOARD_REVS[i]);
            ESP_LOGI(TAG, "board ID: AIN3 %.3f V → level %d = %s, divider x%.3f",
                     v, level, s_board.name, s_board.volt_ratio);
            return;
        }
    }
    // 不認得：比這份韌體新的硬體、擴充碼（EEPROM，尚未支援）、或讀值落在級距之間。
    // 一律退回舊板係數並標成未知，讓 /status 與 OLED 顯示出來 —— 不要猜。
    s_board.name  = "unknown";
    s_board.known = false;
    ESP_LOGW(TAG, "board ID: AIN3 %.3f V → level %d not known to this firmware — "
             "using V1.1/V1.2 divider; update the firmware", v, level);
}

esp_err_t adc_driver_init(void)
{
    // Verify ADS1115 is reachable by writing and reading config register
    uint16_t cfg = ADS_CFG_PGA_4V | ADS_CFG_DR_128 | ADS_CFG_COMP_DIS;
    esp_err_t ret = ads_write_config(cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not found at 0x%02X: %s", ADS_ADDR, esp_err_to_name(ret));
        return ret;
    }
    detect_board();
    return ESP_OK;
}

const adc_board_t *adc_driver_board(void)
{
    return &s_board;
}

// 最後一次有效讀值；I2C 失敗時沿用，避免瞬間掉到 0V
static float s_last_voltage    = 0.0f;
static float s_last_cp_voltage = 0.0f;
static uint32_t s_fail_count   = 0;

uint32_t adc_driver_fail_count(void) { return s_fail_count; }

float adc_driver_read_voltage(void)
{
    // AIN0-AIN1 differential → apply the board's resistor divider
    float adc_v;
    if (!ads_read(ADS_CFG_MUX_01, &adc_v)) {
        if ((++s_fail_count % 100u) == 1u)
            ESP_LOGW(TAG, "ADS1115 read failed (%lu) — holding last value",
                     (unsigned long)s_fail_count);
        return s_last_voltage;
    }
    s_last_voltage = adc_v * s_board.volt_ratio;
    return s_last_voltage;
}

float adc_driver_read_cp_voltage(void)
{
    // AIN2 single-ended → apply resistor divider
    float adc_v;
    if (!ads_read(ADS_CFG_MUX_2G, &adc_v)) {
        if ((++s_fail_count % 100u) == 1u)
            ESP_LOGW(TAG, "ADS1115 CP read failed (%lu) — holding last value",
                     (unsigned long)s_fail_count);
        return s_last_cp_voltage;
    }
    float ratio = (ADC_CP_R1_OHM + ADC_CP_R2_OHM) / ADC_CP_R2_OHM;
    s_last_cp_voltage = adc_v * ratio;
    return s_last_cp_voltage;
}
