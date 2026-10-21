#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_PROV_STATE_BOOT = 0,   /* 启动中 */
    WIFI_PROV_STATE_CONNECTING, /* 正在连接已保存的路由器 */
    WIFI_PROV_STATE_PROVISIONING,
    WIFI_PROV_STATE_RUNNING,    /* 已联网；掉线重连时仍是该状态，需结合 is_connected 判断 */
} wifi_prov_state_t;

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
 * @brief 当前配网/连接状态
 */
wifi_prov_state_t wifi_prov_get_state(void);

/**
 * @brief 读取当前 AP 的信号强度
 *
 * 未连接或读取失败时返回 false，rssi 保持原值。
 */
bool wifi_prov_get_rssi(int8_t *rssi);

/**
 * @brief 当前使用（或正在尝试连接）的 SSID
 */
const char *wifi_prov_get_ssid(void);

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
