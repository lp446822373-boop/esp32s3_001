#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS / netif / WiFi，并在后台启动配网或连接已保存的路由器
 *
 * 非阻塞：内部创建任务处理连接与配网。已保存凭证时先尝试 STA 连接，
 * 失败后自动开启 SoftAP 配网热点；无凭证时直接开启配网热点。
 */
esp_err_t wifi_prov_init(void);

/**
 * @brief 当前是否已从路由器获得 IP
 */
bool wifi_prov_is_connected(void);

/**
 * @brief NVS 中是否保存了 WiFi 凭证
 */
bool wifi_prov_has_credentials(void);

/**
 * @brief 清除已保存的 WiFi 凭证（下次启动进入配网模式）
 */
esp_err_t wifi_prov_erase_credentials(void);

#ifdef __cplusplus
}
#endif
