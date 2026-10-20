#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 扫码分帧回调
 *
 * 在扫码接收任务上下文执行，必须快速返回，不要做网络等阻塞操作。
 * truncated 为 true 表示单帧超过内部缓冲被截断，调用方应整条丢弃。
 */
typedef void (*scanner_frame_cb_t)(const uint8_t *data, size_t len,
                                   bool truncated, void *ctx);

/**
 * @brief 初始化扫码枪串口（UART1，只接收）
 *
 * 扫码数据通过 ESP_LOGI 打印到日志控制台，并回调 on_frame（可为 NULL）。
 * 帧以 CR/LF 结束，若 100 ms 内没有新数据，已收到的内容也会作为一帧输出。
 *
 * @param rx_gpio   扫码枪 TX 连接的 ESP32 RX 引脚
 * @param baud_rate 波特率（扫码枪默认 115200）
 * @param on_frame  分帧回调，可为 NULL
 * @param ctx       传给回调的上下文
 */
esp_err_t scanner_init(gpio_num_t rx_gpio, uint32_t baud_rate,
                       scanner_frame_cb_t on_frame, void *ctx);

#ifdef __cplusplus
}
#endif
