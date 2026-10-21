#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2C 总线与 SSD1306 OLED，并在后台启动显示任务
 *
 * 屏幕是可选外设：初始化失败时返回错误，调用方不应因此重启系统。
 * 初始化成功后显示任务会先跑一遍点亮自检图案。
 */
esp_err_t display_init(void);

/**
 * @brief 面板是否已初始化成功
 */
bool display_is_ready(void);

#ifdef __cplusplus
}
#endif
