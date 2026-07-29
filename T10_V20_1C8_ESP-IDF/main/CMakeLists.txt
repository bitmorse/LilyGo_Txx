idf_component_register(
    SRCS
        "app_main.c"
        "st7735.c"
        "buttons.c"
        "wifi_scan.c"
        "power.c"
        "sound.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash esp_timer
)
