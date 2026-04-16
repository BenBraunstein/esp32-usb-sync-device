#pragma once

// Start the MQTT client. Connects to broker, subscribes to sync topics.
// Posts EVENT_MQTT_CONNECTED to state machine on successful connection.
void mqtt_app_start(void);

// Publish a status string to embroidery/status. Thread-safe, callable from any task.
void mqtt_publish_status(const char *status);
