idf_component_register(
    SRCS "network_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_wifi esp_event nvs_flash esp_http_server esp_netif json app_update spi_flash mdns
)