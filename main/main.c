#include "config.h"
#include "nvs_config.h"
#include "led.h"
#include "sd_card.h"
#include "usb_msc.h"
#include "state_machine.h"
#include "app_mqtt.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "main";

// ---- WiFi ------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            led_set_state(LED_STATE_WIFI_CONNECTING);
            esp_wifi_connect();
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        state_machine_post_event(EVENT_WIFI_CONNECTED);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Read credentials (NVS overrides compile-time defaults from .env)
    char ssid[33];
    char password[65];
    nvs_config_get_str("wifi_ssid", WIFI_SSID,     ssid,     sizeof(ssid));
    nvs_config_get_str("wifi_pass", WIFI_PASSWORD,  password, sizeof(password));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, connecting to '%s'", ssid);
}

// ---- app_main --------------------------------------------------------------

void app_main(void)
{
    // 1. Initialize NVS (required before WiFi and config reads)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_config_init();

    // 2. Start LED — shows yellow breathing immediately (WiFi connecting)
    led_init();

    // 3. Initialize SD card (SPI bus + card probe)
    ret = sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed — entering error state");
        led_set_state(LED_STATE_ERROR);
        return;  // Cannot proceed without SD card
    }

    // 4. Initialize USB MSC with the SD card handle
    ret = usb_msc_init(sd_card_get_card());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB MSC init failed — entering error state");
        led_set_state(LED_STATE_ERROR);
        return;
    }

    // Start with VFS mounted (drive hidden from USB host) until state machine
    // explicitly exposes it after WiFi + MQTT are connected.
    usb_msc_mount_for_sync();

    // 5. Start state machine (creates event queue + task)
    state_machine_init();

    // 6. Connect WiFi (event handler will post EVENT_WIFI_CONNECTED)
    wifi_init();

    // 7. Start MQTT (will post EVENT_MQTT_CONNECTED when broker connects)
    mqtt_app_start();

    ESP_LOGI(TAG, "Initialization complete — state machine running");
    // app_main returns; FreeRTOS tasks (state machine, LED, TinyUSB, MQTT) continue
}
