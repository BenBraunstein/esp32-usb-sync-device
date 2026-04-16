#pragma once

#include "esp_err.h"

void nvs_config_init(void);

// Read a string from NVS. Falls back to default_val if key not found.
// Writes into caller-provided buffer. Returns pointer to buf for convenience.
const char *nvs_config_get_str(const char *key, const char *default_val,
                               char *buf, size_t buf_len);

// Write a string to NVS (for future OTA config updates).
esp_err_t nvs_config_set_str(const char *key, const char *value);
