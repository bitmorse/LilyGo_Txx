idf_component_register(
    SRCS
        "app_main.c"
        "st7735.c"
        "buttons.c"
        "wifi_scan.c"
        "power.c"
        "sound.c"
        "i2c_bus.c"
        "imu.c"
        "provisioning.c"
        "airport.c"
        "audio.c"
        "lvgl_port.c"
        "ui_menu.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash esp_timer spi_flash esp_partition
             wifi_provisioning bt esp_http_client esp-tls json esp_driver_dac lvgl
)
