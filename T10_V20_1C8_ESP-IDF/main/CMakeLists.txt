idf_component_register(
    SRCS
        "app_main.c"
        "st7735.c"
        "buttons.c"
        "wifi_scan.c"
        "sound.c"
        "i2c_bus.c"
        "imu.c"
        "provisioning.c"
        "wifi_creds.c"
        "netmgr.c"
        "airport.c"
        "audio.c"
        "radio.c"
        "sdcard.c"
        "mcap.c"
        "accel_encode.c"
        "viblog.c"
        "manifest.c"
        "manifest_filter.c"
        "fileid.c"
        "filesrv.c"
        "apmode.c"
        "uartrx.c"
        "uartrx_ring.c"
        "uartrx_sm.c"
        "uartrx_rec.c"
        "settings.c"
        "blesync.c"
        "lvgl_port.c"
        "ui_menu.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash esp_timer spi_flash esp_partition
             bt esp_http_client esp-tls json esp_driver_dac
             esp-libhelix-mp3 lvgl fatfs sdmmc esp_driver_sdspi esp_driver_spi
             esp_http_server mdns mbedtls esp_app_format
)
