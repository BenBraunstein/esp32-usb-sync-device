#pragma once

#include "esp_err.h"

// Execute a full sync cycle: fetch manifest, compare against SD card,
// download new/changed files. Returns ESP_OK on success.
esp_err_t sync_run(void);
