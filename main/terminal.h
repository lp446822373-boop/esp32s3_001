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

/* ---------- 最近一次扫码事件（供显示屏轮询） ---------- */

typedef enum {
    TERMINAL_EVENT_NONE = 0,
    TERMINAL_EVENT_PROCESSING,  /* 已收到扫码，正在上报 */
    TERMINAL_EVENT_SUCCESS,
    TERMINAL_EVENT_FAILED,
    TERMINAL_EVENT_PENDING,     /* 需要员工在手机上确认 */
    TERMINAL_EVENT_UNCONFIRMED, /* 断网、超时、HTTP/响应异常、未配置 */
} terminal_event_t;

typedef struct {
    terminal_event_t event;
    uint32_t seq;              /* 每次更新 +1，显示层据此判断是否有新结果 */
    int http_status;           /* 0 表示未收到 HTTP 响应 */
    long long amount_cents;    /* -1 表示服务端未返回 */
    char code[24];
    char message[96];
    char employee_name[48];
    char meal_name[48];
    char transaction_no[48];
    char token_tail[16];
    int64_t updated_us;
} terminal_snapshot_t;

/**
 * @brief 读取最近一次扫码事件快照（可从任意任务调用）
 *
 * @return true 表示至少产生过一次事件
 */
bool terminal_get_snapshot(terminal_snapshot_t *out);

#ifdef __cplusplus
}
#endif
