#pragma once

typedef enum {
    STATE_IDLE,
    STATE_MOUNTING,
    STATE_MOUNTED,
    STATE_UNMOUNTING,
    STATE_SYNCING,
    STATE_ERROR,
} device_state_t;

typedef enum {
    EVENT_WIFI_CONNECTED,
    EVENT_MQTT_CONNECTED,
    EVENT_SYNC_REQUESTED,
    EVENT_FORCE_SYNC,
    EVENT_MOUNT_COMPLETE,
    EVENT_UNMOUNT_COMPLETE,
    EVENT_SYNC_COMPLETE,
    EVENT_SYNC_FAILED,
} state_event_t;

void state_machine_init(void);
void state_machine_post_event(state_event_t event);
device_state_t state_machine_get_state(void);
