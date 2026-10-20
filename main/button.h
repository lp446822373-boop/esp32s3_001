#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct button_dev *button_handle_t;

typedef struct {
    gpio_num_t gpio;
    void (*on_short_press)(void *ctx);
    void (*on_long_press)(void *ctx);
    void *ctx;
    uint32_t long_press_ms;  /* 长按判定阈值，0 表示默认 800 ms */
    uint32_t long_repeat_ms; /* 长按连发间隔，0 表示长按只触发一次 */
} button_config_t;

/**
 * @brief 创建一个按键实例（低电平按下，内部上拉）
 *
 * 每个实例有独立的轮询任务和状态，可同时创建多个按键。
 *
 * @param cfg 按键配置
 * @param out 返回的按键句柄
 */
esp_err_t button_create(const button_config_t *cfg, button_handle_t *out);

#ifdef __cplusplus
}
#endif
