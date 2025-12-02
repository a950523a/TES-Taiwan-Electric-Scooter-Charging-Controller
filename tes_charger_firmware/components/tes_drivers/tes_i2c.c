#include "tes_i2c.h"
#include "board_bsp.h"
#include "esp_log.h"

static const char *TAG = "TES_I2C";

// 實體化 Bus Handle
i2c_master_bus_handle_t bus_handle = NULL;

esp_err_t tes_i2c_init(void) {
    // 如果已經初始化過，直接返回成功
    if (bus_handle != NULL) return ESP_OK;

    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_MAIN.port_num,
        .scl_io_num = BOARD_I2C_MAIN.scl_io,
        .sda_io_num = BOARD_I2C_MAIN.sda_io,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_LOGI(TAG, "Initializing I2C Master Bus (New API)...");
    return i2c_new_master_bus(&i2c_mst_config, &bus_handle);
}