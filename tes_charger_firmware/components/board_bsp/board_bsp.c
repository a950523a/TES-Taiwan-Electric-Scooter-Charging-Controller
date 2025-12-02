#include "board_bsp.h"
#include "driver/gpio.h"

esp_err_t tes_io_init(const tes_io_t *io, bool is_output) {
    if (io->type == IO_TYPE_INTERNAL_GPIO) {
        gpio_config_t conf = {
            .pin_bit_mask = (1ULL << io->gpio_num),
            .mode = is_output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = (io->pull_mode == IO_PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = (io->pull_mode == IO_PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        return gpio_config(&conf);
    }
    return ESP_OK;
}

esp_err_t tes_io_set_level(const tes_io_t *io, int level) {
    if (io->type == IO_TYPE_INTERNAL_GPIO) {
        // 如果是 Active Low (inverted=true)，輸入 1 時實際輸出 0
        int physical_level = io->inverted ? !level : level;
        return gpio_set_level((gpio_num_t)io->gpio_num, physical_level);
    }
    return ESP_FAIL;
}

esp_err_t tes_io_toggle(const tes_io_t *io) {
    if (io->type == IO_TYPE_INTERNAL_GPIO) {
        int current = gpio_get_level((gpio_num_t)io->gpio_num);
        return gpio_set_level((gpio_num_t)io->gpio_num, !current);
    }
    return ESP_FAIL;
}

int tes_io_get_level(const tes_io_t *io) {
    if (io->type == IO_TYPE_INTERNAL_GPIO) {
        int raw_level = gpio_get_level((gpio_num_t)io->gpio_num);
        // 如果 inverted=true (Active Low)，讀到 0 代表按下(True)，讀到 1 代表放開(False)
        return io->inverted ? !raw_level : raw_level;
    }
    return 0; // 預設回傳 0
}