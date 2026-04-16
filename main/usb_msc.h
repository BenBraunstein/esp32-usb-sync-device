#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

// Initialize TinyUSB MSC with the given SD card handle. Call once at boot.
esp_err_t usb_msc_init(sdmmc_card_t *card);

// Unmount VFS and expose SD card to USB host (PE900 sees the drive).
esp_err_t usb_msc_expose_to_host(void);

// Mount VFS for local file access (sync can read/write files).
esp_err_t usb_msc_mount_for_sync(void);

// Unmount the direct SD card VFS after sync is complete.
esp_err_t usb_msc_unmount_sync(void);
