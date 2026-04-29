#pragma once

#include "esp_err.h"
#include <stddef.h>

// Sync result counters — populated by sync_run()
typedef struct {
    int total;       // total files in manifest
    int downloaded;  // files downloaded (new or changed)
    int skipped;     // files already up to date
    int errors;      // files that failed to download
} sync_result_t;

// Execute a full sync cycle: fetch manifest, compare against SD card,
// download new/changed files. Returns ESP_OK on success.
// If result is non-NULL, it is populated with sync statistics.
esp_err_t sync_run(sync_result_t *result);
