#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

// Initialize SPI bus and probe the SD card. Call once at boot.
esp_err_t sd_card_init(void);

// Return the low-level card handle (for passing to TinyUSB MSC init).
sdmmc_card_t *sd_card_get_card(void);
