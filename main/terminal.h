#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动终端客户端：扫码上报任务、心跳任务与待确认结果轮询任务
 *
 * 配置由 terminal_config 提供；未配置时任务仍运行，扫码会被提示未配置。
 */
esp_err_t terminal_init(void);

/**
 * @brief 扫码枪分帧回调，直接注册给 scanner_init()
 *
 * 只做校验和入队，不阻塞扫码任务。token 超过 128 字节、被截断、不是 pmt_
 * 前缀或含控制字符时整条丢弃。
 */
void terminal_on_scan_frame(const uint8_t *data, size_t len, bool truncated, void *ctx);

#ifdef __cplusplus
}
#endif
