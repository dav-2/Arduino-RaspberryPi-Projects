

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_wifi
        esp_system
        esp_netif
        esp_event
        nvs_flash
        lwip
)

