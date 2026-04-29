#include "state_machine.h"
#include "config.h"
#include "led.h"
#include "app_mqtt.h"
#include "usb_msc.h"
#include "sync.h"
#include "tusb_msc_storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <stdatomic.h>

static const char *TAG = "state_machine";

static QueueHandle_t s_event_queue;
static _Atomic device_state_t s_state = STATE_IDLE;
static bool s_wifi_connected;
static bool s_mqtt_connected;
static bool s_pending_sync;
static bool s_boot_sync_done;   // true after the first boot sync has been triggered
static int  s_retry_count;
static TimerHandle_t s_retry_timer;
static TimerHandle_t s_debounce_timer;
static TimerHandle_t s_boot_sync_timer; // delays boot sync to let USB host enumerate

// ---- helpers ---------------------------------------------------------------

static const char *state_name(device_state_t st)
{
    switch (st) {
    case STATE_IDLE:       return "idle";
    case STATE_MOUNTING:   return "mounting";
    case STATE_MOUNTED:    return "mounted";
    case STATE_UNMOUNTING: return "unmounting";
    case STATE_SYNCING:    return "syncing";
    case STATE_ERROR:      return "error";
    }
    return "unknown";
}

static const char *event_name(state_event_t ev)
{
    switch (ev) {
    case EVENT_WIFI_CONNECTED:  return "WIFI_CONNECTED";
    case EVENT_MQTT_CONNECTED:  return "MQTT_CONNECTED";
    case EVENT_SYNC_REQUESTED:  return "SYNC_REQUESTED";
    case EVENT_FORCE_SYNC:      return "FORCE_SYNC";
    case EVENT_MOUNT_COMPLETE:  return "MOUNT_COMPLETE";
    case EVENT_UNMOUNT_COMPLETE:return "UNMOUNT_COMPLETE";
    case EVENT_SYNC_COMPLETE:   return "SYNC_COMPLETE";
    case EVENT_SYNC_FAILED:     return "SYNC_FAILED";
    }
    return "UNKNOWN";
}

static void enter_state(device_state_t new_state)
{
    device_state_t old = atomic_load(&s_state);
    atomic_store(&s_state, new_state);
    ESP_LOGI(TAG, "%s -> %s", state_name(old), state_name(new_state));

    // Update LED
    switch (new_state) {
    case STATE_IDLE:       led_set_state(LED_STATE_WIFI_CONNECTING); break;
    case STATE_MOUNTING:   led_set_state(LED_STATE_MOUNTING);        break;
    case STATE_MOUNTED:
        if (s_pending_sync) {
            led_set_state(LED_STATE_PENDING_SYNC);
        } else {
            led_set_state(LED_STATE_MOUNTED_IDLE);
        }
        break;
    case STATE_UNMOUNTING: led_set_state(LED_STATE_UNMOUNTING);      break;
    case STATE_SYNCING:    led_set_state(LED_STATE_SYNCING);         break;
    case STATE_ERROR:      led_set_state(LED_STATE_ERROR);           break;
    }

    // Publish MQTT status
    mqtt_publish_status(state_name(new_state));
}

// ---- timer callbacks -------------------------------------------------------

static void retry_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Retry timer fired, attempting sync again");
    state_machine_post_event(EVENT_SYNC_REQUESTED);
}

static void debounce_timer_cb(TimerHandle_t timer)
{
    // Debounce period elapsed — now actually start the unmount→sync flow
    device_state_t st = atomic_load(&s_state);
    if (st == STATE_MOUNTED) {
        ESP_LOGI(TAG, "Debounce complete, beginning unmount for sync");
        enter_state(STATE_UNMOUNTING);
        usb_msc_mount_for_sync();
    }
}

static void boot_sync_timer_cb(TimerHandle_t timer)
{
    // Delayed boot sync — fires 5 seconds after first mount to give the USB
    // host time to fully enumerate the drive before we take it back for sync.
    device_state_t st = atomic_load(&s_state);
    if (st == STATE_MOUNTED) {
        ESP_LOGI(TAG, "Boot sync timer fired, triggering initial sync");
        state_machine_post_event(EVENT_SYNC_REQUESTED);
    }
}

// ---- event handler ---------------------------------------------------------

static void handle_event(state_event_t event)
{
    device_state_t st = atomic_load(&s_state);
    ESP_LOGD(TAG, "Event %s in state %s", event_name(event), state_name(st));

    switch (st) {

    case STATE_IDLE:
        if (event == EVENT_WIFI_CONNECTED) {
            s_wifi_connected = true;
            led_set_state(LED_STATE_MQTT_CONNECTING);
        }
        if (event == EVENT_MQTT_CONNECTED) {
            s_mqtt_connected = true;
        }
        // Transition once both are up
        if (s_wifi_connected && s_mqtt_connected && st == STATE_IDLE) {
            enter_state(STATE_MOUNTING);
            usb_msc_expose_to_host();
        }
        break;

    case STATE_MOUNTING:
        if (event == EVENT_MOUNT_COMPLETE) {
            enter_state(STATE_MOUNTED);
            // Auto-sync on first boot — start a 5-second timer to give the
            // USB host time to fully enumerate the drive before we take it back.
            if (!s_boot_sync_done) {
                s_boot_sync_done = true;
                ESP_LOGI(TAG, "Boot sync: scheduling initial sync in 5s");
                xTimerStart(s_boot_sync_timer, 0);
            }
        }
        break;

    case STATE_MOUNTED:
        if (event == EVENT_SYNC_REQUESTED || event == EVENT_FORCE_SYNC) {
            s_pending_sync = true;
            // Unmount from USB host and mount VFS for sync immediately
            enter_state(STATE_UNMOUNTING);
            usb_msc_mount_for_sync();
        }
        break;

    case STATE_UNMOUNTING:
        if (event == EVENT_UNMOUNT_COMPLETE) {
            // VFS is now mounted for local access — run sync
            enter_state(STATE_SYNCING);
            sync_result_t sync_result = {0};
            esp_err_t err = sync_run(&sync_result);
            // Always publish last_synced — even if all files were skipped
            mqtt_publish_last_synced(sync_result.total, sync_result.downloaded,
                                     sync_result.skipped, sync_result.errors);
            if (err == ESP_OK) {
                state_machine_post_event(EVENT_SYNC_COMPLETE);
            } else {
                state_machine_post_event(EVENT_SYNC_FAILED);
            }
        }
        break;

    case STATE_SYNCING:
        if (event == EVENT_SYNC_COMPLETE) {
            s_pending_sync = false;
            s_retry_count = 0;
            led_set_state(LED_STATE_SYNC_COMPLETE);
            vTaskDelay(pdMS_TO_TICKS(350));
            // Unmount the direct SD card VFS before re-exposing to USB host
            usb_msc_unmount_sync();
            enter_state(STATE_MOUNTING);
            usb_msc_expose_to_host();
        }
        if (event == EVENT_SYNC_FAILED) {
            s_retry_count++;
            if (s_retry_count >= MAX_RETRY_COUNT) {
                char msg[64];
                snprintf(msg, sizeof(msg), "error: sync failed after %d retries", MAX_RETRY_COUNT);
                mqtt_publish_status(msg);
                s_pending_sync = false;
                s_retry_count = 0;
                usb_msc_unmount_sync();
                enter_state(STATE_MOUNTING);
                usb_msc_expose_to_host();
            } else {
                ESP_LOGW(TAG, "Sync failed, retry %d/%d in %d ms",
                         s_retry_count, MAX_RETRY_COUNT, RETRY_DELAY_MS);
                enter_state(STATE_ERROR);
                xTimerStart(s_retry_timer, 0);
            }
        }
        break;

    case STATE_ERROR:
        // Retry timer posts SYNC_REQUESTED — VFS should still be mounted from
        // the previous sync attempt, so we can retry directly.
        if (event == EVENT_SYNC_REQUESTED || event == EVENT_FORCE_SYNC) {
            // Re-mount VFS in case it was lost (idempotent if already mounted)
            esp_err_t mount_err = tinyusb_msc_storage_mount(SD_MOUNT_POINT);
            if (mount_err != ESP_OK) {
                ESP_LOGW(TAG, "VFS re-mount returned %s (may already be mounted)",
                         esp_err_to_name(mount_err));
            }
            if (event == EVENT_FORCE_SYNC) s_retry_count = 0;
            enter_state(STATE_SYNCING);
            sync_result_t sync_result = {0};
            esp_err_t err = sync_run(&sync_result);
            mqtt_publish_last_synced(sync_result.total, sync_result.downloaded,
                                     sync_result.skipped, sync_result.errors);
            if (err == ESP_OK) {
                state_machine_post_event(EVENT_SYNC_COMPLETE);
            } else {
                state_machine_post_event(EVENT_SYNC_FAILED);
            }
        }
        if (event == EVENT_SYNC_COMPLETE) {
            s_pending_sync = false;
            s_retry_count = 0;
            led_set_state(LED_STATE_SYNC_COMPLETE);
            vTaskDelay(pdMS_TO_TICKS(350));
            enter_state(STATE_MOUNTING);
            usb_msc_expose_to_host();
        }
        break;
    }
}

// ---- task ------------------------------------------------------------------

static void state_machine_task(void *arg)
{
    state_event_t event;
    while (1) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            handle_event(event);
        }
    }
}

// ---- public API ------------------------------------------------------------

void state_machine_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(state_event_t));
    assert(s_event_queue);

    s_retry_timer = xTimerCreate("retry", pdMS_TO_TICKS(RETRY_DELAY_MS),
                                 pdFALSE, NULL, retry_timer_cb);
    s_debounce_timer = xTimerCreate("debounce", pdMS_TO_TICKS(SYNC_DEBOUNCE_MS),
                                    pdFALSE, NULL, debounce_timer_cb);
    // 5-second one-shot timer — fires once after first mount to trigger boot sync
    s_boot_sync_timer = xTimerCreate("boot_sync", pdMS_TO_TICKS(5000),
                                     pdFALSE, NULL, boot_sync_timer_cb);

    xTaskCreate(state_machine_task, "state_machine", 16384, NULL, 5, NULL);
    ESP_LOGI(TAG, "State machine started");
}

void state_machine_post_event(state_event_t event)
{
    if (xQueueSend(s_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropped %s", event_name(event));
    }
}

device_state_t state_machine_get_state(void)
{
    return atomic_load(&s_state);
}
