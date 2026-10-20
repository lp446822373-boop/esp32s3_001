#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 服务端基础地址，例如 https://canteen.example.com/canteen */
#define TERMINAL_URL_MAX 160
/* 终端 key，后台创建终端时提供，形如 trm_... */
#define TERMINAL_KEY_MAX 96

typedef struct {
    char base_url[TERMINAL_URL_MAX + 1];
    char key[TERMINAL_KEY_MAX + 1];
} terminal_config_t;

/**
 * @brief 初始化 NVS 并加载终端配置到内存缓存
 *
 * 可先于 wifi_prov_init() 调用。配置缺失不算错误，terminal_config_is_complete()
 * 返回 false，扫码/心跳会提示未配置。
 */
esp_err_t terminal_config_init(void);

/**
 * @brief 拷贝当前配置（加锁）
 */
void terminal_config_get(terminal_config_t *out);

/**
 * @brief 基础地址和终端 key 是否都已配置且格式合法
 */
bool terminal_config_is_complete(const terminal_config_t *cfg);

/**
 * @brief 更新配置并写入 NVS
 *
 * 空字符串表示保持原值（便于配网页面不回显终端 key）。两个字段合并后仍不完整
 * 返回 ESP_ERR_NOT_FOUND，格式错误返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t terminal_config_update(const char *base_url, const char *key);

/**
 * @brief 校验地址格式（要求 http:// 或 https:// 开头，末尾斜杠允许）
 */
bool terminal_config_url_valid(const char *url);

/**
 * @brief 校验终端 key 格式（要求 trm_ 开头，可打印 ASCII）
 */
bool terminal_config_key_valid(const char *key);

#ifdef __cplusplus
}
#endif
