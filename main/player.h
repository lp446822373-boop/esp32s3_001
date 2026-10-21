#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

/**
 * @brief 初始化 UART0 并连接播放器模块（115200 8N1）
 */
esp_err_t player_init(void);

/**
 * @brief 播放/暂停切换
 *
 * 播放中 -> 暂停；暂停中 -> 继续；停止 -> 从第 1 首开始播放。
 */
void player_toggle_play_pause(void);

/**
 * @brief 下一首（会触发播放）
 */
void player_next(void);

/**
 * @brief 播放指定曲目（1 起）,index 为 0 时不做任何操作
 *
 * 用于终端结果提示音；会打断当前播放内容。
 */
void player_play_track(uint16_t index);

/**
 * @brief 音频模块是否在本次上电后有过应答
 */
bool player_is_available(void);

/**
 * @brief 最后收到模块应答的时间（微秒），0 表示上电后从未收到
 *
 * 模块每 2 秒会被查询一次状态，可用于判断当前是否在线。
 */
int64_t player_get_last_reply_us(void);

/**
 * @brief 音量加 1 级（0~31，超出范围不发送）
 */
void player_volume_up(void);

/**
 * @brief 音量减 1 级（0~31，超出范围不发送）
 */
void player_volume_down(void);

/**
 * @brief 当前音量（0~31）
 */
uint8_t player_get_volume(void);

player_state_t player_get_state(void);

const char *player_state_name(player_state_t state);

#ifdef __cplusplus
}
#endif
