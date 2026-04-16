#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_config";
static nvs_handle_t s_nvs_handle;

void nvs_config_init(void)
{
    esp_err_t err = nvs_open("config", NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'config': %s", esp_err_to_name(err));
    }
}

const char *nvs_config_get_str(const char *key, const char *default_val,
                               char *buf, size_t buf_len)
{
    size_t required = buf_len;
    esp_err_t err = nvs_get_str(s_nvs_handle, key, buf, &required);
    if (err == ESP_OK) {
        return buf;
    }

    // Key not in NVS — use compile-time default
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS read error for '%s': %s", key, esp_err_to_name(err));
    }
    strncpy(buf, default_val, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return buf;
}

esp_err_t nvs_config_set_str(const char *key, const char *value)
{
    esp_err_t err = nvs_set_str(s_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write error for '%s': %s", key, esp_err_to_name(err));
        return err;
    }
    return nvs_commit(s_nvs_handle);
}
