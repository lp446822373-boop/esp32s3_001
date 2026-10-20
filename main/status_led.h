#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_PROVISIONING, /* 蓝色慢闪：配网热点已开启 */
    STATUS_LED_CONNECTING,   /* 黄色常亮：正在连接路由器 */
    STATUS_LED_CONNECTED,    /* 绿色亮 1.5 s 后熄灭 */
    STATUS_LED_RECONNECTING, /* 橙色慢闪：运行中掉线重连 */
    STATUS_LED_ERROR,        /* 红色快闪 */
} status_led_mode_t;

typedef enum {
    STATUS_LED_RESULT_SUCCESS = 0, /* 绿灯常亮 */
    STATUS_LED_RESULT_FAILED,      /* 红灯常亮 */
    STATUS_LED_RESULT_PENDING,     /* 黄灯慢闪 */
    STATUS_LED_RESULT_UNCONFIRMED, /* 红灯快闪 */
} status_led_result_t;

/**
 * @brief 初始化板载 RGB LED，执行上电自检并启动状态灯任务
 */
esp_err_t status_led_init(void);

/**
 * @brief 切换状态灯显示模式（可从任意任务调用）
 */
void status_led_set_mode(status_led_mode_t mode);

/**
 * @brief 临时显示扫码结果，到期后自动恢复 WiFi 状态显示
 */
void status_led_show_result(status_led_result_t result);

#ifdef __cplusplus
}
#endif
