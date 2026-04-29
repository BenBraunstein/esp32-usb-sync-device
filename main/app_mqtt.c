#include "app_mqtt.h"
#include "config.h"
#include "nvs_config.h"
#include "state_machine.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;

// ---- Home Assistant MQTT Discovery -----------------------------------------

#define MQTT_TOPIC_AVAIL    "embroidery/availability"

// Publish HA discovery messages so the device auto-appears in Home Assistant.
// Creates: a sensor (status), online/offline availability, and sync buttons.
static void publish_ha_discovery(void)
{
    // Shared device + availability block for all entities
    #define HA_DEVICE_JSON \
        "\"dev\":{" \
            "\"ids\":[\"" HA_DEVICE_ID "\"]," \
            "\"name\":\"" HA_DEVICE_NAME "\"," \
            "\"mf\":\"Espressif\"," \
            "\"mdl\":\"ESP32-S3 Super Mini\"," \
            "\"sw\":\"1.0.0\"" \
        "}," \
        "\"avty_t\":\"" MQTT_TOPIC_AVAIL "\","  \
        "\"pl_avail\":\"online\"," \
        "\"pl_not_avail\":\"offline\""

    // 1. Status sensor — shows current state (mounted, syncing, error, etc.)
    const char *sensor_topic = HA_DISCOVERY_PREFIX "/sensor/" HA_DEVICE_ID "/status/config";
    const char *sensor_payload =
        "{"
            "\"name\":\"Status\","
            "\"uniq_id\":\"" HA_DEVICE_ID "_status\","
            "\"stat_t\":\"" MQTT_TOPIC_STATUS "\","
            "\"icon\":\"mdi:sync\","
            HA_DEVICE_JSON
        "}";
    esp_mqtt_client_publish(s_client, sensor_topic, sensor_payload, 0, 1, 1);

    // 2. Force Sync button
    const char *button_topic = HA_DISCOVERY_PREFIX "/button/" HA_DEVICE_ID "/force_sync/config";
    const char *button_payload =
        "{"
            "\"name\":\"Force Sync\","
            "\"uniq_id\":\"" HA_DEVICE_ID "_force_sync\","
            "\"cmd_t\":\"" MQTT_TOPIC_FORCE "\","
            "\"pl_prs\":\"1\","
            "\"icon\":\"mdi:cloud-sync\","
            HA_DEVICE_JSON
        "}";
    esp_mqtt_client_publish(s_client, button_topic, button_payload, 0, 1, 1);

    // 3. Sync button
    const char *sync_button_topic = HA_DISCOVERY_PREFIX "/button/" HA_DEVICE_ID "/sync/config";
    const char *sync_button_payload =
        "{"
            "\"name\":\"Sync\","
            "\"uniq_id\":\"" HA_DEVICE_ID "_sync\","
            "\"cmd_t\":\"" MQTT_TOPIC_SYNC "\","
            "\"pl_prs\":\"1\","
            "\"icon\":\"mdi:file-sync\","
            HA_DEVICE_JSON
        "}";
    esp_mqtt_client_publish(s_client, sync_button_topic, sync_button_payload, 0, 1, 1);

    // 4. Last Synced timestamp sensor
    const char *last_sync_topic = HA_DISCOVERY_PREFIX "/sensor/" HA_DEVICE_ID "/last_synced/config";
    const char *last_sync_payload =
        "{"
            "\"name\":\"Last Synced\","
            "\"uniq_id\":\"" HA_DEVICE_ID "_last_synced\","
            "\"stat_t\":\"" MQTT_TOPIC_LAST_SYNC "\","
            "\"icon\":\"mdi:clock-check\","
            "\"dev_cla\":\"timestamp\","
            HA_DEVICE_JSON
        "}";
    esp_mqtt_client_publish(s_client, last_sync_topic, last_sync_payload, 0, 1, 1);

    // Publish online availability (LWT handles offline automatically)
    esp_mqtt_client_publish(s_client, MQTT_TOPIC_AVAIL, "online", 0, 1, 1);

    ESP_LOGI(TAG, "Published Home Assistant MQTT discovery messages");

    #undef HA_DEVICE_JSON
}

// ---- MQTT event handler ----------------------------------------------------

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_SYNC, 1);
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_FORCE, 1);

        // Publish HA discovery on every connect (handles HA restarts)
        publish_ha_discovery();

        state_machine_post_event(EVENT_MQTT_CONNECTED);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker (auto-reconnect active)");
        break;

    case MQTT_EVENT_DATA:
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

// ---- public API ------------------------------------------------------------

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
        // Last Will and Testament — broker publishes "offline" if ESP32 disconnects
        .session.last_will = {
            .topic = MQTT_TOPIC_AVAIL,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", uri);
}

void mqtt_publish_status(const char *status)
{
    if (s_client) {
        // QoS 1, retain=1 so HA always has the latest state even after restart
        esp_mqtt_client_publish(s_client, MQTT_TOPIC_STATUS, status, 0, 1, 1);
    }
}

void mqtt_publish_last_synced(int total, int downloaded, int skipped, int errors)
{
    if (!s_client) return;

    // Format current time as ISO 8601 (e.g. "2026-04-16T12:34:56+00:00")
    // Home Assistant's timestamp device class requires ISO 8601 format.
    char ts[32];
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);

    if (t && now > 1000000000) {
        // SNTP time is available (epoch > year 2001)
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S+00:00", t);
    } else {
        // No NTP sync — use uptime in seconds as a fallback label
        snprintf(ts, sizeof(ts), "uptime:%lld", (long long)now);
    }

    // Publish ISO 8601 timestamp for HA timestamp sensor
    esp_mqtt_client_publish(s_client, MQTT_TOPIC_LAST_SYNC, ts, 0, 1, 1);

    // Also publish a human-readable summary as the status
    char summary[128];
    snprintf(summary, sizeof(summary),
             "synced at %s | %d total, %d new, %d skipped, %d errors",
             ts, total, downloaded, skipped, errors);
    ESP_LOGI(TAG, "Published last_synced: %s", summary);
}
