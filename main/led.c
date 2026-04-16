#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include <math.h>
#include <stdatomic.h>

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static _Atomic led_state_t s_state = LED_STATE_WIFI_CONNECTING;
static int64_t s_state_entry_us;
static _Atomic int s_paused = 0;  // when non-zero, led_task skips updates

// ---- helpers ---------------------------------------------------------------

static float sine_breath(int64_t elapsed_us, int period_ms)
{
    float t = (float)(elapsed_us / 1000) / (float)period_ms;
    return (sinf(t * 2.0f * M_PI) + 1.0f) / 2.0f;
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void set_off(void)
{
    led_strip_clear(s_strip);
}

// ---- pattern functions -----------------------------------------------------

static void pattern_breathing(int64_t elapsed_us, int period_ms,
                              uint8_t r, uint8_t g, uint8_t b)
{
    float bright = sine_breath(elapsed_us, period_ms);
    set_rgb((uint8_t)(r * bright), (uint8_t)(g * bright), (uint8_t)(b * bright));
}

static void pattern_fast_pulse(int64_t elapsed_us,
                               uint8_t r, uint8_t g, uint8_t b)
{
    // 3 blinks per second: 333ms period, on for first half
    int phase = (int)((elapsed_us / 1000) % 333);
    if (phase < 166) {
        set_rgb(r, g, b);
    } else {
        set_off();
    }
}

static void pattern_solid(uint8_t r, uint8_t g, uint8_t b)
{
    set_rgb(r, g, b);
}

static void pattern_pending_sync(int64_t elapsed_us)
{
    // Solid green at idle brightness, with a brief cyan flash every 3 seconds
    int cycle_ms = (int)((elapsed_us / 1000) % 3000);
    if (cycle_ms < 150) {
        // Cyan flash
        set_rgb(0, 220, 220);
    } else {
        // Idle green
        float scale = (float)LED_BRIGHTNESS_IDLE / 255.0f;
        set_rgb(0, (uint8_t)(200 * scale), 0);
    }
}

static void pattern_fade_off(int64_t elapsed_us)
{
    // Blue fading to off over 1 second
    int ms = (int)(elapsed_us / 1000);
    if (ms >= 1000) {
        set_off();
        return;
    }
    float bright = 1.0f - ((float)ms / 1000.0f);
    set_rgb(0, (uint8_t)(80 * bright), (uint8_t)(255 * bright));
}

static void pattern_double_blink(int64_t elapsed_us)
{
    // Blink 100ms on, 100ms off, 100ms on, 500ms off — total 800ms cycle
    int phase = (int)((elapsed_us / 1000) % 800);
    if (phase < 100 || (phase >= 200 && phase < 300)) {
        set_rgb(255, 0, 0);
    } else {
        set_off();
    }
}

// ---- task ------------------------------------------------------------------

static void led_task(void *arg)
{
    led_state_t prev_state = LED_STATE_WIFI_CONNECTING;
    s_state_entry_us = esp_timer_get_time();

    while (1) {
        // If paused (debug_flash is using the strip), skip updates
        if (atomic_load(&s_paused)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        led_state_t state = atomic_load(&s_state);

        // Reset entry timestamp on state change
        if (state != prev_state) {
            s_state_entry_us = esp_timer_get_time();
            prev_state = state;
        }

        int64_t elapsed = esp_timer_get_time() - s_state_entry_us;

        switch (state) {
        case LED_STATE_WIFI_CONNECTING:
            // Yellow breathing, 2s cycle
            pattern_breathing(elapsed, 2000, 255, 180, 0);
            break;

        case LED_STATE_MQTT_CONNECTING:
            // Purple breathing, 2s cycle (distinct from yellow WiFi)
            pattern_breathing(elapsed, 2000, 180, 0, 255);
            break;

        case LED_STATE_MOUNTING:
            // Blue fast pulse, 3 blinks/sec
            pattern_fast_pulse(elapsed, 0, 80, 255);
            break;

        case LED_STATE_MOUNTED_IDLE: {
            // Solid green at 30% brightness
            float scale = (float)LED_BRIGHTNESS_IDLE / 255.0f;
            pattern_solid(0, (uint8_t)(200 * scale), 0);
            break;
        }

        case LED_STATE_PENDING_SYNC:
            pattern_pending_sync(elapsed);
            break;

        case LED_STATE_UNMOUNTING:
            pattern_fade_off(elapsed);
            break;

        case LED_STATE_SYNCING:
            // Cyan fast breathing, 0.5s cycle
            pattern_breathing(elapsed, 500, 0, 220, 220);
            break;

        case LED_STATE_SYNC_COMPLETE: {
            // White flash for 300ms, then auto-transition to MOUNTING
            int ms = (int)(elapsed / 1000);
            if (ms < 300) {
                set_rgb(255, 255, 255);
            } else {
                atomic_store(&s_state, LED_STATE_MOUNTING);
            }
            break;
        }

        case LED_STATE_ERROR:
            pattern_double_blink(elapsed);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz refresh
    }
}

// ---- public API ------------------------------------------------------------

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);

    xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);
    ESP_LOGI(TAG, "LED task started");
}

void led_set_state(led_state_t state)
{
    atomic_store(&s_state, state);
}

led_strip_handle_t led_debug_get_strip(void)
{
    return s_strip;
}

void led_pause(void)
{
    atomic_store(&s_paused, 1);
    // Give led_task time to finish any in-progress RMT transaction
    vTaskDelay(pdMS_TO_TICKS(40));
}

void led_resume(void)
{
    atomic_store(&s_paused, 0);
}
