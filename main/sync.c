#include "sync.h"
#include "config.h"
#include "nvs_config.h"
#include "app_mqtt.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>

static const char *TAG = "sync";

#define HTTP_BUF_SIZE   4096
#define MANIFEST_MAX    (64 * 1024)  // 64 KB max manifest size

// ---- helpers ---------------------------------------------------------------

// Recursively create directories for a file path.
// Given "/sdcard/designs/sub/file.pes", creates /sdcard/designs/ and /sdcard/designs/sub/.
static void mkdir_p(const char *filepath)
{
    char tmp[256];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    // Walk past the mount point prefix
    char *p = tmp + strlen(SD_MOUNT_POINT) + 1;
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);  // ignore EEXIST
            *p = '/';
        }
    }
}

// HTTP GET into a heap buffer. Caller must free() the returned pointer.
// Returns NULL on failure. *out_len receives the body length.
static char *http_get_to_buffer(const char *url, int *out_len, int max_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    // Allocate buffer — use content_length if known, otherwise grow dynamically
    int alloc_size = (content_length > 0 && content_length < max_len)
                     ? content_length + 1
                     : max_len;
    char *buf = malloc(alloc_size);
    if (!buf) {
        ESP_LOGE(TAG, "Malloc failed for %d bytes", alloc_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total = 0;
    int len;
    while ((len = esp_http_client_read(client, buf + total, alloc_size - total - 1)) > 0) {
        total += len;
        if (total >= alloc_size - 1) break;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_len = total;
    return buf;
}

// Stream an HTTP GET response directly to a file. Returns ESP_OK on success.
static esp_err_t http_get_to_file(const char *url, const char *filepath)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Write to .tmp file for atomic rename
    char tmp_path[280];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    mkdir_p(filepath);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s", tmp_path, strerror(errno));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) {
        fclose(fp);
        unlink(tmp_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int len;
    esp_err_t result = ESP_OK;
    while ((len = esp_http_client_read(client, buf, HTTP_BUF_SIZE)) > 0) {
        if (fwrite(buf, 1, len, fp) != (size_t)len) {
            ESP_LOGE(TAG, "Write error for %s", tmp_path);
            result = ESP_FAIL;
            break;
        }
    }

    free(buf);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || len < 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    // Atomic rename
    if (rename(tmp_path, filepath) != 0) {
        ESP_LOGE(TAG, "Rename %s -> %s failed: %s", tmp_path, filepath, strerror(errno));
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ---- main sync logic -------------------------------------------------------

esp_err_t sync_run(void)
{
    char server_ip[64];
    char server_port[8];
    nvs_config_get_str("unraid_ip",   UNRAID_IP,   server_ip,   sizeof(server_ip));
    nvs_config_get_str("unraid_port", UNRAID_PORT,  server_port, sizeof(server_port));

    // 1. Fetch manifest
    char manifest_url[128];
    snprintf(manifest_url, sizeof(manifest_url),
             "http://%s:%s/manifest.json", server_ip, server_port);

    ESP_LOGI(TAG, "Fetching manifest from %s", manifest_url);

    int manifest_len = 0;
    char *manifest_json = http_get_to_buffer(manifest_url, &manifest_len, MANIFEST_MAX);
    if (!manifest_json) {
        ESP_LOGE(TAG, "Failed to fetch manifest");
        return ESP_FAIL;
    }

    // 2. Parse JSON
    cJSON *root = cJSON_Parse(manifest_json);
    free(manifest_json);
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Invalid manifest JSON");
        if (root) cJSON_Delete(root);
        return ESP_FAIL;
    }

    int total_files = cJSON_GetArraySize(root);
    int downloaded = 0;
    int skipped = 0;
    int errors = 0;

    ESP_LOGI(TAG, "Manifest has %d files", total_files);

    // 3. Process each entry
    cJSON *entry;
    cJSON_ArrayForEach(entry, root) {
        cJSON *path_json = cJSON_GetObjectItem(entry, "path");
        cJSON *size_json = cJSON_GetObjectItem(entry, "size");

        if (!cJSON_IsString(path_json) || !cJSON_IsNumber(size_json)) {
            ESP_LOGW(TAG, "Skipping malformed manifest entry");
            continue;
        }

        const char *rel_path = path_json->valuestring;
        size_t expected_size = (size_t)size_json->valuedouble;

        // Build full SD card path
        char full_path[280];
        snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, rel_path);

        // Check if file exists and matches size
        struct stat st;
        if (stat(full_path, &st) == 0 && (size_t)st.st_size == expected_size) {
            skipped++;
            continue;
        }

        // Download needed
        char file_url[384];
        snprintf(file_url, sizeof(file_url),
                 "http://%s:%s/files/%s", server_ip, server_port, rel_path);

        ESP_LOGI(TAG, "Downloading: %s (%zu bytes)", rel_path, expected_size);

        esp_err_t err = http_get_to_file(file_url, full_path);
        if (err == ESP_OK) {
            downloaded++;
        } else {
            ESP_LOGE(TAG, "Failed to download %s", rel_path);
            errors++;
        }
    }

    cJSON_Delete(root);

    // 4. Report results
    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg),
             "sync complete: %d downloaded, %d skipped, %d errors",
             downloaded, skipped, errors);
    ESP_LOGI(TAG, "%s", status_msg);
    mqtt_publish_status(status_msg);

    return (errors > 0) ? ESP_FAIL : ESP_OK;
}
