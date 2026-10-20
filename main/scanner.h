#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化扫码枪串口（UART1，只接收）
 *
 * 扫码数据通过 ESP_LOGI 打印到日志控制台。帧以 CR/LF 结束，
 * 若 100 ms 内没有新数据，已收到的内容也会作为一帧输出。
 *
 * @param rx_gpio   扫码枪 TX 连接的 ESP32 RX 引脚
 * @param baud_rate 波特率（扫码枪默认 115200）
 */
esp_err_t scanner_init(gpio_num_t rx_gpio, uint32_t baud_rate);

#ifdef __cplusplus
}
#endif
