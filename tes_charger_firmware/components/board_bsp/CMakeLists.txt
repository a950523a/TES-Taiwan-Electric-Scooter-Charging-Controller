idf_component_register(
    SRCS "board_bsp.c" "board_v1.c"
    INCLUDE_DIRS "include"
    REQUIRES driver  # 依賴 ESP-IDF 的 driver component
)