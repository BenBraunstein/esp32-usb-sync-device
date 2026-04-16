#include "usb_msc.h"
#include "config.h"
#include "state_machine.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

static const char *TAG = "usb_msc";

// ---- mount change callback -------------------------------------------------

static void mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event->mount_changed_data.is_mounted) {
        // VFS mounted → app (sync) has file access
        ESP_LOGI(TAG, "VFS mounted (available for sync)");
        state_machine_post_event(EVENT_UNMOUNT_COMPLETE);
    } else {
        // VFS unmounted → USB host has the drive
        ESP_LOGI(TAG, "VFS unmounted (USB host has drive)");
        state_machine_post_event(EVENT_MOUNT_COMPLETE);
    }
}

// ---- USB descriptors -------------------------------------------------------

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define EPNUM_MSC_OUT       0x01
#define EPNUM_MSC_IN        0x81

static uint8_t const s_msc_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static tusb_desc_device_t const s_device_desc = {
    .bLength            = sizeof(s_device_desc),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   // Espressif VID
    .idProduct          = 0x4002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},  // Language: English (US)
    "Espressif",                 // Manufacturer
    "Embroidery File Sync",      // Product
    "000001",                    // Serial
};

// ---- public API ------------------------------------------------------------

esp_err_t usb_msc_init(sdmmc_card_t *card)
{
    // 1. Register the SD card as MSC backing storage
    const tinyusb_msc_sdmmc_config_t sdmmc_config = {
        .card = card,
        .callback_mount_changed = mount_changed_cb,
        .mount_config = {
            .max_files = 5,
            .format_if_mount_failed = true,
            .allocation_unit_size = 0, // default
        },
    };

    esp_err_t ret = tinyusb_msc_storage_init_sdmmc(&sdmmc_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Install the TinyUSB driver with our descriptors
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_string_desc,
        .string_descriptor_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .configuration_descriptor = s_msc_fs_config_desc,
    };

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB MSC initialized");
    return ESP_OK;
}

esp_err_t usb_msc_expose_to_host(void)
{
    ESP_LOGI(TAG, "Exposing SD card to USB host");
    return tinyusb_msc_storage_unmount();
}

esp_err_t usb_msc_mount_for_sync(void)
{
    ESP_LOGI(TAG, "Mounting VFS for sync access");
    esp_err_t ret = tinyusb_msc_storage_mount(SD_MOUNT_POINT);
    if (ret != ESP_OK) {
        // May already be mounted (e.g. after init) — not fatal
        ESP_LOGW(TAG, "VFS mount returned %s (may already be mounted)", esp_err_to_name(ret));
    }
    return ESP_OK;
}
