#include "app_mqtt.h"
#include "config.h"
#include "nvs_config.h"
#include "state_machine.h"
#include "esp_log.h"
#include "mqtt_client.h"  // ESP-IDF MQTT client
#include <string.h>

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_SYNC, 1);
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_FORCE, 1);
        state_machine_post_event(EVENT_MQTT_CONNECTED);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker (auto-reconnect active)");
        break;

    case MQTT_EVENT_DATA:
        // Null-terminate topic for comparison
        if (event->topic_len > 0) {
            if (event->topic_len == strlen(MQTT_TOPIC_FORCE) &&
                strncmp(event->topic, MQTT_TOPIC_FORCE, event->topic_len) == 0) {
                ESP_LOGI(TAG, "Received force_sync");
                state_machine_post_event(EVENT_FORCE_SYNC);
            } else if (event->topic_len == strlen(MQTT_TOPIC_SYNC) &&
                       strncmp(event->topic, MQTT_TOPIC_SYNC, event->topic_len) == 0) {
                ESP_LOGI(TAG, "Received sync request");
                state_machine_post_event(EVENT_SYNC_REQUESTED);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: %d", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

void mqtt_app_start(void)
{
    char broker_ip[64];
    char broker_port[8];
    char username[64];
    char password[64];

    nvs_config_get_str("mqtt_ip",   MQTT_BROKER_IP,   broker_ip,   sizeof(broker_ip));
    nvs_config_get_str("mqtt_port", MQTT_BROKER_PORT,  broker_port, sizeof(broker_port));
    nvs_config_get_str("mqtt_user", MQTT_USERNAME,     username,    sizeof(username));
    nvs_config_get_str("mqtt_pass", MQTT_PASSWORD,     password,    sizeof(password));

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%s", broker_ip, broker_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .credentials.client_id = MQTT_CLIENT_ID,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", uri);
}

void mqtt_publish_status(const char *status)
{
    if (s_client) {
        esp_mqtt_client_publish(s_client, MQTT_TOPIC_STATUS, status, 0, 1, 0);
    }
}
