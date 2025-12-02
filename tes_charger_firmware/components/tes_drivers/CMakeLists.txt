idf_component_register(
    SRCS "tes_can.c" "tes_i2c.c" "tes_adc.c" "tes_u8g2_hal.c" "tes_oled.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board_bsp u8g2 # 依賴 driver 和 bsp
)