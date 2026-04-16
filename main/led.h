#pragma once

typedef enum {
    LED_STATE_WIFI_CONNECTING,   // Yellow slow breathing, 2s cycle
    LED_STATE_MQTT_CONNECTING,   // Orange slow breathing, 2s cycle
    LED_STATE_MOUNTING,          // Blue fast pulse, 3 blinks/sec
    LED_STATE_MOUNTED_IDLE,      // Solid green, 30% brightness
    LED_STATE_PENDING_SYNC,      // Solid green + brief cyan flash every 3s
    LED_STATE_UNMOUNTING,        // Blue fade to off over 1s
    LED_STATE_SYNCING,           // Cyan fast breathing, 0.5s cycle
    LED_STATE_SYNC_COMPLETE,     // White flash 300ms, then auto → MOUNTING
    LED_STATE_ERROR,             // Red double-blink, 500ms pause, repeat
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
