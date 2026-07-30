// microSD card over SPI (FAT filesystem), mounted at /sdcard.
//
// The card slot on this board is wired to its own SPI bus (separate from the TFT
// on SPI2): SCK=14, MOSI=15, MISO=2, CS=13. We use SPI3_HOST so the display is
// never disturbed. Only mounted on demand (vibration logging), not at boot.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Mount the card at /sdcard. Returns true on success. Safe to call repeatedly
// (a second call while already mounted just returns true).
bool sd_mount(void);

// Unmount and release the SPI bus.
void sd_unmount(void);

bool sd_is_mounted(void);

// Free space on the mounted card, in bytes (0 if not mounted / query failed).
uint64_t sd_free_bytes(void);
