idf_component_register(
    SRCS "main.cpp" "js_service.cpp" 
    INCLUDE_DIRS "."
    REQUIRES board_bsp tes_drivers quickjs u8g2 tes_network
)