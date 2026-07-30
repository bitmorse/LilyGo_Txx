#include "sdcard.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd";

// This board's microSD slot (separate SPI bus from the TFT).
#define SD_SCK   14
#define SD_MOSI  15
#define SD_MISO   2
#define SD_CS    13
#define SD_HOST  SPI3_HOST

#define MOUNT_POINT "/sdcard"

static sdmmc_card_t *s_card;
static bool          s_bus_ready;

bool sd_is_mounted(void) { return s_card != NULL; }

bool sd_mount(void)
{
    if (s_card) return true;

    spi_bus_config_t buscfg = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    // NOTE: use SPI_DMA_CH_AUTO, not SDSPI_DEFAULT_DMA. On the ESP32 the latter
    // is a legacy alias that resolves to a *fixed* channel 1 (== HSPI_HOST), which
    // the TFT on SPI2 already owns -> "no available dma channel". AUTO picks the
    // remaining free channel (2) so the display and SD card can share DMA.
    esp_err_t err = spi_bus_initialize(SD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }
    s_bus_ready = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    // Conservative clock; SPI-mode cards on hand-wired slots are happier slower.
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs   = SD_CS;
    slot.host_id   = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // never reformat the user's card
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (card inserted? FAT formatted?)",
                 esp_err_to_name(err));
        spi_bus_free(SD_HOST);
        s_bus_ready = false;
        return false;
    }

    ESP_LOGI(TAG, "mounted %s: %lluMB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return true;
}

void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    if (s_bus_ready) {
        spi_bus_free(SD_HOST);
        s_bus_ready = false;
    }
}

uint64_t sd_free_bytes(void)
{
    if (!s_card) return 0;
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &avail) != ESP_OK) return 0;
    return avail;
}
