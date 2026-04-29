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
#include <ctype.h>

static const char *TAG = "sync";

#define HTTP_BUF_SIZE   4096
#define MANIFEST_MAX    (64 * 1024)  // 64 KB max manifest size

// URL-encode a path component. Spaces become %20, other unsafe chars are encoded.
// Slashes are preserved (they're path separators, not encoded).
// Returns number of bytes written (excluding null terminator), or -1 if buffer too small.
static int url_encode_path(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;

    for (const char *s = src; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            if (di + 1 >= dst_size) return -1;
            dst[di++] = c;
        } else {
            if (di + 3 >= dst_size) return -1;
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    if (di >= dst_size) return -1;
    dst[di] = '\0';
    return (int)di;
}

// ---- helpers ---------------------------------------------------------------

// Recursively create directories for a file path.
// Given "/sdcard/Boutique Orders/10 8 25/file.pes", creates each directory level.
static esp_err_t mkdir_p(const char *filepath)
{
    const char *last_slash = strrchr(filepath, '/');
    if (!last_slash || last_slash == filepath) {
        return ESP_OK;
    }

    size_t dir_len = last_slash - filepath;
    char dir_path[512];
    if (dir_len >= sizeof(dir_path)) {
        ESP_LOGE(TAG, "Directory path too long: %zu", dir_len);
        return ESP_FAIL;
    }
    memcpy(dir_path, filepath, dir_len);
    dir_path[dir_len] = '\0';

    size_t mount_len = strlen(SD_MOUNT_POINT);
    if (dir_len <= mount_len) {
        return ESP_OK;
    }

    // Try creating the full directory path first (works if parent exists)
    if (mkdir(dir_path, 0755) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    // Full path failed — create each level one at a time
    for (size_t i = mount_len + 1; i <= dir_len; i++) {
        if (i == dir_len || dir_path[i] == '/') {
            char saved = dir_path[i];
            dir_path[i] = '\0';

            struct stat dst;
            if (stat(dir_path, &dst) == 0) {
                dir_path[i] = saved;
                continue;
            }

            int ret = mkdir(dir_path, 0755);
            if (ret != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: '%s' errno=%d", dir_path, errno);
                dir_path[i] = saved;
                return ESP_FAIL;
            }

            dir_path[i] = saved;
        }
    }
    return ESP_OK;
}

// HTTP GET into a heap buffer. Caller must free() the returned pointer.
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

    if (mkdir_p(filepath) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    unlink(filepath);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s", filepath, strerror(errno));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) {
        fclose(fp);
        unlink(filepath);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int len;
    esp_err_t result = ESP_OK;
    while ((len = esp_http_client_read(client, buf, HTTP_BUF_SIZE)) > 0) {
        if (fwrite(buf, 1, len, fp) != (size_t)len) {
            ESP_LOGE(TAG, "Write error for %s", filepath);
            result = ESP_FAIL;
            break;
        }
    }

    free(buf);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || len < 0) {
        unlink(filepath);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ---- main sync logic -------------------------------------------------------

esp_err_t sync_run(sync_result_t *result)
{
    // Initialize result counters
    sync_result_t local_result = {0};

    // Verify VFS mount point is accessible
    struct stat mount_st;
    if (stat(SD_MOUNT_POINT, &mount_st) != 0) {
        ESP_LOGE(TAG, "Mount point %s not accessible (errno=%d)", SD_MOUNT_POINT, errno);
        mqtt_publish_status("error: SD card not accessible");
        return ESP_FAIL;
    }

    char server_ip[64];
    char server_port[8];
    nvs_config_get_str("unraid_ip",   UNRAID_IP,   server_ip,   sizeof(server_ip));
    nvs_config_get_str("unraid_port", UNRAID_PORT,  server_port, sizeof(server_port));

    // 1. Fetch manifest
    char manifest_url[128];
    snprintf(manifest_url, sizeof(manifest_url),
             "http://%s:%s/manifest.json?ext=%s",
             server_ip, server_port, MANIFEST_EXT_FILTER);

    ESP_LOGI(TAG, "Fetching manifest from %s", manifest_url);

    int manifest_len = 0;
    char *manifest_json = http_get_to_buffer(manifest_url, &manifest_len, MANIFEST_MAX);
    if (!manifest_json) {
        ESP_LOGE(TAG, "Failed to fetch manifest");
        mqtt_publish_status("error: manifest fetch failed");
        return ESP_FAIL;
    }

    // 2. Parse JSON
    cJSON *root = cJSON_Parse(manifest_json);
    free(manifest_json);
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Invalid manifest JSON");
        mqtt_publish_status("error: invalid manifest");
        if (root) cJSON_Delete(root);
        return ESP_FAIL;
    }

    local_result.total = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Manifest has %d files", local_result.total);

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
            local_result.skipped++;
            continue;
        }

        // Download needed — URL-encode the path to handle spaces and special chars
        char encoded_path[512];
        if (url_encode_path(rel_path, encoded_path, sizeof(encoded_path)) < 0) {
            ESP_LOGE(TAG, "Path too long to encode: %s", rel_path);
            local_result.errors++;
            continue;
        }

        char file_url[640];
        snprintf(file_url, sizeof(file_url),
                 "http://%s:%s/files/%s", server_ip, server_port, encoded_path);

        ESP_LOGI(TAG, "Downloading: %s (%zu bytes)", rel_path, expected_size);

        esp_err_t err = http_get_to_file(file_url, full_path);
        if (err == ESP_OK) {
            local_result.downloaded++;
        } else {
            ESP_LOGE(TAG, "Failed to download %s", rel_path);
            local_result.errors++;
            // Stop after 3 errors to keep output manageable
            if (local_result.errors >= 3) {
                ESP_LOGW(TAG, "Stopping sync early after %d errors", local_result.errors);
                break;
            }
        }
    }

    cJSON_Delete(root);

    // 4. Report results
    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg),
             "sync complete: %d downloaded, %d skipped, %d errors",
             local_result.downloaded, local_result.skipped, local_result.errors);
    ESP_LOGI(TAG, "%s", status_msg);
    mqtt_publish_status(status_msg);

    // Return result to caller if requested
    if (result) {
        *result = local_result;
    }

    return (local_result.errors > 0) ? ESP_FAIL : ESP_OK;
}
