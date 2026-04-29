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
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "led_strip.h"

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

// Brief solid color flash for debugging boot sequence (visible on LED).
// Each init step gets a unique color so we can see where it crashes.
static void debug_flash(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_handle_t strip = led_debug_get_strip();
    if (strip) {
        led_pause();  // stop led_task from touching the RMT channel
        led_strip_set_pixel(strip, 0, r, g, b);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(400));
        led_strip_clear(strip);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_resume();  // let led_task run again
    } else {
        // Strip not initialized yet, just delay
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

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
    ESP_LOGI(TAG, "NVS config init OK");

    // 2. Start LED — shows yellow breathing immediately (WiFi connecting)
    led_init();
    ESP_LOGI(TAG, "LED init OK");

    debug_flash(255, 0, 0);       // RED = LED init done, about to init SD

    // 3. Initialize SD card (SPI bus + card probe)
    led_set_state(LED_STATE_SYNCING);  // cyan = "about to init SD"
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Starting SD card init...");
    ret = sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed — entering error state");
        led_set_state(LED_STATE_ERROR);
        return;
    }
    ESP_LOGI(TAG, "SD card init OK");

    debug_flash(0, 255, 0);       // GREEN = SD init done, about to init USB MSC

    led_set_state(LED_STATE_MOUNTING);  // blue = "about to init USB MSC"
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. Initialize USB MSC with the SD card handle
    ESP_LOGI(TAG, "Starting USB MSC init...");
    ret = usb_msc_init(sd_card_get_card());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB MSC init failed — entering error state");
        led_set_state(LED_STATE_ERROR);
        return;
    }
    ESP_LOGI(TAG, "USB MSC init OK");

    debug_flash(0, 0, 255);       // BLUE = USB MSC done, about to init state machine

    // 5. Start state machine (creates event queue + task)
    state_machine_init();
    ESP_LOGI(TAG, "State machine init OK");

    debug_flash(255, 255, 0);     // YELLOW = state machine done, about to init WiFi

    // 6. Set LED to WiFi connecting and start WiFi
    led_set_state(LED_STATE_WIFI_CONNECTING);

    wifi_init();
    ESP_LOGI(TAG, "WiFi init OK");

    debug_flash(255, 0, 255);     // MAGENTA = WiFi done, about to init MQTT

    // 7. Start SNTP for accurate timestamps (used by last_synced sensor in HA)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");

    // 8. Start MQTT (will post EVENT_MQTT_CONNECTED when broker connects)
    mqtt_app_start();
    ESP_LOGI(TAG, "Initialization complete — state machine running");
}
