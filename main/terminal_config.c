#include "terminal_config.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TERMINAL_NVS_NAMESPACE "terminal_cfg"
#define TERMINAL_NVS_KEY_URL "url"
#define TERMINAL_NVS_KEY_TOKEN "key"

static const char *TAG = "terminal_cfg";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static terminal_config_t s_cfg;
static bool s_initialized;

/* URL / key 只允许可打印 ASCII，避免空白、控制字符和非 ASCII 进入 HTTP 请求 */
static bool is_printable_ascii(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7F) {
            return false;
        }
    }
    return true;
}

bool terminal_config_url_valid(const char *url)
{
    if (url == NULL) {
        return false;
    }

    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/') {
        len--;
    }
    if (len < 8 || len > TERMINAL_URL_MAX) {
        return false;
    }

    const char *host;
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else {
        return false;
    }
    if (*host == '\0' || *host == '/') {
        return false;
    }
    /* 基础地址只用于拼接接口路径，不接受查询串或片段 */
    if (strchr(url, '?') != NULL || strchr(url, '#') != NULL) {
        return false;
    }
    return is_printable_ascii(url, len);
}

bool terminal_config_key_valid(const char *key)
{
    if (key == NULL) {
        return false;
    }

    size_t len = strlen(key);
    if (len < 4 || len > TERMINAL_KEY_MAX) {
        return false;
    }
    if (strncmp(key, "trm_", 4) != 0) {
        return false;
    }
    return is_printable_ascii(key, len);
}

/* 校验并去掉末尾斜杠，便于直接拼接 /api/v1/... 路径 */
static bool copy_normalized_url(const char *src, char *dst, size_t dst_size)
{
    if (!terminal_config_url_valid(src)) {
        return false;
    }

    size_t len = strlen(src);
    while (len > 0 && src[len - 1] == '/') {
        len--;
    }
    if (len >= dst_size) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

esp_err_t terminal_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    terminal_config_t cfg = {0};
    nvs_handle_t handle;
    err = nvs_open(TERMINAL_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        char url[TERMINAL_URL_MAX + 1] = {0};
        size_t len = sizeof(url);
        if (nvs_get_str(handle, TERMINAL_NVS_KEY_URL, url, &len) == ESP_OK) {
            if (!copy_normalized_url(url, cfg.base_url, sizeof(cfg.base_url))) {
                ESP_LOGW(TAG, "ignoring invalid saved server URL");
            }
        }

        char key[TERMINAL_KEY_MAX + 1] = {0};
        len = sizeof(key);
        if (nvs_get_str(handle, TERMINAL_NVS_KEY_TOKEN, key, &len) == ESP_OK) {
            if (terminal_config_key_valid(key)) {
                memcpy(cfg.key, key, strlen(key) + 1);
            } else {
                ESP_LOGW(TAG, "ignoring invalid saved terminal key");
            }
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "open NVS namespace failed: %s", esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_cfg = cfg;
    portEXIT_CRITICAL(&s_lock);
    s_initialized = true;

    ESP_LOGI(TAG, "terminal config %s, server: %s",
             terminal_config_is_complete(&cfg) ? "ready" : "incomplete",
             cfg.base_url[0] != '\0' ? cfg.base_url : "(not set)");
    return ESP_OK;
}

void terminal_config_get(terminal_config_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
}

bool terminal_config_is_complete(const terminal_config_t *cfg)
{
    return cfg != NULL && terminal_config_url_valid(cfg->base_url) &&
           terminal_config_key_valid(cfg->key);
}

esp_err_t terminal_config_update(const char *base_url, const char *key)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    terminal_config_t next;
    terminal_config_get(&next);

    if (base_url != NULL && base_url[0] != '\0') {
        if (!copy_normalized_url(base_url, next.base_url, sizeof(next.base_url))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (key != NULL && key[0] != '\0') {
        if (!terminal_config_key_valid(key)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(next.key, key, strlen(key) + 1);
    }

    if (!terminal_config_is_complete(&next)) {
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TERMINAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open NVS for write failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(handle, TERMINAL_NVS_KEY_URL, next.base_url);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, TERMINAL_NVS_KEY_TOKEN, next.key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save terminal config failed: %s", esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_cfg = next;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "terminal config saved, server: %s", next.base_url);
    return ESP_OK;
}
