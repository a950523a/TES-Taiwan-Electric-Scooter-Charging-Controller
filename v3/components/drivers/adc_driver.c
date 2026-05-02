#include "drivers/adc_driver.h"
#include "hal/hal_i2c.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "adc_driver";

// ADS1115 register addresses
#define ADS_ADDR          0x48
#define ADS_REG_CONV      0x00
#define ADS_REG_CFG       0x01

// Config word fields (written MSB first)
// OS=1 (start single), MUX, PGA=001 (±4.096V), MODE=0 (continuous), DR=100 (128SPS), COMP_*=default
#define ADS_CFG_OS        (1u << 15)
#define ADS_CFG_MUX_01    (0b000u << 12)  // differential AIN0-AIN1
#define ADS_CFG_MUX_23    (0b011u << 12)  // differential AIN2-AIN3
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

static int16_t ads_read_conversion(void)
{
    uint8_t reg = ADS_REG_CONV;
    uint8_t raw[2];
    if (hal_i2c_write_read(ADS_ADDR, &reg, 1, raw, 2) != ESP_OK) {
        return 0;
    }
    return (int16_t)((raw[0] << 8) | raw[1]);
}

static float ads_read_differential(uint16_t mux_bits)
{
    uint16_t cfg = ADS_CFG_OS | mux_bits | ADS_CFG_PGA_4V |
                   ADS_CFG_MODE_SINGLE | ADS_CFG_DR_128 | ADS_CFG_COMP_DIS;
    ads_write_config(cfg);
    vTaskDelay(pdMS_TO_TICKS(9));  // 128SPS → ~8ms per sample
    int16_t raw = ads_read_conversion();
    return (float)raw * (ADS_FSR_V / ADS_COUNTS);
}

esp_err_t adc_driver_init(void)
{
    // Verify ADS1115 is reachable by writing and reading config register
    uint16_t cfg = ADS_CFG_PGA_4V | ADS_CFG_DR_128 | ADS_CFG_COMP_DIS;
    esp_err_t ret = ads_write_config(cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not found at 0x%02X: %s", ADS_ADDR, esp_err_to_name(ret));
    }
    return ret;
}

float adc_driver_read_voltage(void)
{
    // AIN0-AIN1 differential → apply resistor divider
    float adc_v = ads_read_differential(ADS_CFG_MUX_01);
    float ratio  = (ADC_VOLT_R1_KOHM + ADC_VOLT_R2_KOHM) / ADC_VOLT_R2_KOHM;
    return adc_v * ratio;
}

float adc_driver_read_cp_voltage(void)
{
    // AIN2-AIN3 differential → apply resistor divider
    float adc_v = ads_read_differential(ADS_CFG_MUX_23);
    float ratio  = (ADC_CP_R1_OHM + ADC_CP_R2_OHM) / ADC_CP_R2_OHM;
    return adc_v * ratio;
}
