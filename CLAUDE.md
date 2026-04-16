# Embroidery Sync Device — ESP32-S3 Project

## Project Overview

This firmware turns an ESP32-S3 Super Mini into a WiFi-connected USB mass storage device
that stays permanently plugged into a Brother PE900 embroidery machine. It automatically
syncs embroidery design files from a network share on an Unraid server to a local SD card,
then presents that SD card to the embroidery machine as a USB flash drive.

The sync is additive only (never deletes from SD card) and incremental — only files that
are missing or differ in size/mtime are downloaded, similar to `aws s3 sync`.

A WS2812 RGB LED provides visual status feedback at a glance for all device states.

---

## Hardware

- **Board:** ESP32-S3 Super Mini
- **Storage:** SPI micro SD card module
- **LED:** WS2812 RGB LED (onboard on ESP32-S3 Super Mini, no wiring needed)
- **USB:** Onboard USB-C port (GPIO19 = D−, GPIO20 = D+) connected via USB-C to USB-A
cable directly to the PE900. This is both the MSC data connection and the power source.

### GPIO Pin Constants

All GPIO assignments are defined as constants in `config.h`. Do not use magic numbers.

```c
// SD Card (SPI)
#define PIN_SD_MOSI     11
#define PIN_SD_MISO     13
#define PIN_SD_SCK      12
#define PIN_SD_CS       10

// WS2812 onboard LED (GPIO 48 on ESP32-S3 Super Mini)
#define PIN_LED         48

// USB OTG (TinyUSB) — do not reassign, hardware-fixed on S3
#define PIN_USB_DN      19
#define PIN_USB_DP      20
```

---

## LED Status Indicator

The WS2812 LED communicates device state visually. All patterns run on a dedicated
FreeRTOS task (`led_task`) so they never block the main state machine.

### LED Pattern Definitions


| State                      | Color (RGB)           | Pattern                             | Description                          |
| -------------------------- | --------------------- | ----------------------------------- | ------------------------------------ |
| WiFi connecting            | Yellow (255, 180, 0)  | Slow breathing, 2s cycle            | Waking up, not yet online            |
| MQTT connecting            | Purple (180, 0, 255)  | Slow breathing, 2s cycle            | WiFi up, broker connecting           |
| Mounting                   | Blue (0, 80, 255)     | Fast pulse, 3 blinks/sec            | Initializing USB MSC                 |
| Mounted, idle              | Green (0, 200, 0)     | Solid, 30% brightness               | Ready, drive visible to machine      |
| Mounted, machine accessing | Green (0, 255, 0)     | Rapid random flicker                | Drive activity (read/write by PE900) |
| Pending sync queued        | Green (0, 200, 0)     | Solid + brief cyan flash every 3s   | Mounted but sync is waiting          |
| Unmounting                 | Blue (0, 80, 255)     | Single slow fade to off over 1s     | Handing off to sync                  |
| Syncing                    | Cyan (0, 220, 220)    | Fast breathing, 0.5s cycle          | Downloading files                    |
| Sync complete              | White (255, 255, 255) | Single 300ms flash, then → Mounting | Success confirmation                 |
| Error                      | Red (255, 0, 0)       | Double-blink, 500ms pause, repeat   | Needs attention                      |


### LED Implementation Notes

- Brightness for "solid idle" is 30% of full green to be subtle, not blinding
- Breathing pattern uses a sine curve for smooth ramp (not linear)
- "Machine accessing" flicker is triggered by `tud_msc_read10_cb` / `tud_msc_write10_cb`
callbacks — set a flag that the LED task reads
- LED task runs at lowest priority so it never competes with sync or USB
- Use `led_set_state(LED_STATE_xxx)` from any task — LED task polls state and drives pattern
- Library: `led_strip` driver from ESP-IDF component registry (supports WS2812 via RMT)

---

## Architecture

### State Machine

The firmware runs a single state machine:

```
STATES: IDLE, MOUNTING, MOUNTED, UNMOUNTING, SYNCING, ERROR

Transitions:
  IDLE         → MOUNTING   : on boot after WiFi + MQTT connect
  MOUNTING     → MOUNTED    : TinyUSB MSC mount complete
  MOUNTED      → UNMOUNTING : force_sync received OR pending_sync=true on USB eject callback
  UNMOUNTING   → SYNCING    : USB host has released the drive
  SYNCING      → MOUNTING   : sync complete
  SYNCING      → ERROR      : HTTP/SD failure after MAX_RETRY_COUNT attempts
  ERROR        → SYNCING    : retry after RETRY_DELAY_MS
  any state    → UNMOUNTING : force_sync MQTT message received

Flags:
  pending_sync: set when embroidery/sync received while in MOUNTED state.
                Checked on USB eject callback — if true, go to SYNCING instead of MOUNTING.
```

### Sync Logic

1. HTTP GET `http://[UNRAID_IP]:[UNRAID_PORT]/manifest.json`
2. Parse JSON array of `{ "path": "relative/path/file.pes", "size": 48200, "mtime": 1710489600 }`
3. For each entry:
  - Check if file exists on SD card at same relative path
  - If missing OR size differs OR mtime differs → download
  - If identical → skip
4. Download via HTTP GET `http://[UNRAID_IP]:[UNRAID_PORT]/files/[path]`
5. Write to SD card preserving full directory structure (create dirs as needed)
6. After all files processed → publish status and transition to MOUNTING

Never delete files from the SD card.

### MQTT Topics


| Topic                   | Direction      | Payload | Behavior                                                          |
| ----------------------- | -------------- | ------- | ----------------------------------------------------------------- |
| `embroidery/sync`       | Broker → ESP32 | any     | Sync if idle/unmounted; set pending_sync if mounted               |
| `embroidery/force_sync` | Broker → ESP32 | any     | Immediately unmount, sync, remount regardless of state            |
| `embroidery/status`     | ESP32 → Broker | string  | State updates: "mounted", "syncing", "unmounting", "error: [msg]" |

**Trigger source:** A Node-RED flow running on Unraid watches for file changes and
publishes to `embroidery/sync` and `embroidery/force_sync` via the MQTT broker.

---

## Configuration

All credentials and network settings live in `config.h` as `#define` constants.
Additionally, all values can be overridden at runtime via NVS (key-value store).
On first boot with no NVS values, compile-time defaults are used.

```c
// WiFi
#define WIFI_SSID           "YOUR_SSID_HERE"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD_HERE"

// MQTT Broker (Home Assistant)
#define MQTT_BROKER_IP      "YOUR_HA_IP_HERE"         // e.g. "192.168.1.100"
#define MQTT_BROKER_PORT    1883
#define MQTT_USERNAME       "YOUR_MQTT_USERNAME_HERE"
#define MQTT_PASSWORD       "YOUR_MQTT_PASSWORD_HERE"
#define MQTT_CLIENT_ID      "embroidery-sync-device"

// Unraid HTTP manifest server
#define UNRAID_IP           "YOUR_UNRAID_IP_HERE"     // e.g. "192.168.1.50"
#define UNRAID_PORT         8765

// Sync behavior
#define MAX_RETRY_COUNT     3
#define RETRY_DELAY_MS      10000
#define SYNC_DEBOUNCE_MS    2000   // wait after MQTT message before starting sync

// SD card mount point
#define SD_MOUNT_POINT      "/sdcard"

// LED
#define LED_BRIGHTNESS_IDLE    76    // 30% of 255
#define LED_BRIGHTNESS_MAX     255
```

---

## File Structure

```
/
├── CMakeLists.txt
├── sdkconfig.defaults
├── CLAUDE.md
├── manifest_server/
│   ├── server.py              # FastAPI manifest + file server (runs on Unraid)
│   ├── requirements.txt
│   └── Dockerfile
└── main/
    ├── CMakeLists.txt
    ├── config.h               # All constants and credentials
    ├── main.c                 # App entry point, WiFi init, state machine loop
    ├── state_machine.c/.h     # State definitions and transition logic
    ├── usb_msc.c/.h           # TinyUSB MSC init, mount/unmount callbacks
    ├── sd_card.c/.h           # SPI SD init, file ops, stat-based comparison
    ├── sync.c/.h              # Manifest fetch, diff logic, file download
    ├── mqtt_client.c/.h       # MQTT connect, subscribe, publish, callbacks
    ├── nvs_config.c/.h        # NVS read/write for runtime config override
    └── led.c/.h               # WS2812 LED task, pattern definitions, state mapping
```

---

## Framework & Libraries

- **Framework:** ESP-IDF v5.x (not Arduino)
- **USB MSC:** TinyUSB (bundled with ESP-IDF), `CONFIG_TINYUSB_MSC_ENABLED=y`
- **SD card:** `driver/sdspi_host.h`, `esp_vfs_fat.h`
- **MQTT:** `mqtt_client.h` (ESP-IDF built-in)
- **HTTP client:** `esp_http_client.h` (ESP-IDF built-in)
- **JSON parsing:** `cJSON` (bundled with ESP-IDF)
- **NVS:** `nvs_flash.h` (ESP-IDF built-in)
- **WS2812 LED:** `led_strip` component (ESP-IDF component registry), RMT peripheral

### sdkconfig.defaults

```
CONFIG_TINYUSB_MSC_ENABLED=y
CONFIG_TINYUSB_MSC_BUFSIZE=4096
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_LED_STRIP_RMT_DEFAULT_MEM_BLOCK_SYMBOLS=48
```

---

## Manifest Server (Unraid)

`manifest_server/server.py` is a FastAPI app that:

- Scans the configured root directory recursively
- Serves `GET /manifest.json` — full file list with relative path, size, mtime
- Serves `GET /files/{path}` — raw file download
- Runs on port 8765

It is deployed as a Docker container on Unraid with the embroidery share mounted read-only.

---

## Important Constraints

- GPIO19 and GPIO20 must never be used for anything other than USB D−/D+
- The SD card must be fully unmounted from the VFS before TinyUSB can expose it as MSC,
and must be remounted to VFS before sync can write to it
- TinyUSB MSC callbacks (`tud_msc_start_stop_cb`) are used to detect host eject events
- Sync must be atomic per-file: write to a `.tmp` file, rename on success, delete on failure
- All file paths from the manifest use forward slashes; normalize on SD write
- LED task must be lowest FreeRTOS priority and must not block on anything

