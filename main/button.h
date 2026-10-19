#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化按键（低电平按下，内部上拉）
 *
 * @param gpio          按键引脚
 * @param on_short_press 短按回调（松开时触发）
 * @param on_long_press  长按回调（按住达到阈值时触发一次）
 */
esp_err_t button_init(gpio_num_t gpio,
                      void (*on_short_press)(void),
                      void (*on_long_press)(void));

#ifdef __cplusplus
}
#endif
