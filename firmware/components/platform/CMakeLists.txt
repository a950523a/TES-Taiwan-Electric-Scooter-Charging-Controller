idf_component_register(
    SRCS
        "esp32/platform_esp32.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_timer
)
