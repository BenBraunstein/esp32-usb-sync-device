#include "sd_card.h"
#include "config.h"
#include "esp_log.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <stdlib.h>

static const char *TAG = "sd_card";

static sdmmc_card_t *s_card;
static sdspi_dev_handle_t s_sdspi_handle;

esp_err_t sd_card_init(void)
{
    esp_err_t ret;

    // 1. Initialize the SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Attach the SD card to the SPI bus
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = SPI2_HOST;

    ret = sdspi_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDSPI host init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sdspi_host_init_device(&slot_config, &s_sdspi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDSPI device init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Probe and initialize the card
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_sdspi_handle;

    s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (!s_card) {
        ESP_LOGE(TAG, "Failed to allocate sdmmc_card_t");
        return ESP_ERR_NO_MEM;
    }

    ret = sdmmc_card_init(&host, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        free(s_card);
        s_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card initialized (%llu MB)",
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));

    return ESP_OK;
}

sdmmc_card_t *sd_card_get_card(void)
{
    return s_card;
}
