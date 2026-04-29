#pragma once

// Start the MQTT client. Connects to broker, subscribes to sync topics.
// Posts EVENT_MQTT_CONNECTED to state machine on successful connection.
void mqtt_app_start(void);

// Publish a status string to embroidery/status. Thread-safe, callable from any task.
void mqtt_publish_status(const char *status);

// Publish the current time as an ISO 8601 timestamp to embroidery/last_synced,
// along with a sync summary (total, downloaded, skipped, errors).
// Call after every sync (success or failure). Uses SNTP time if available.
void mqtt_publish_last_synced(int total, int downloaded, int skipped, int errors);
