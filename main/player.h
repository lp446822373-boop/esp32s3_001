#pragma once

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

player_state_t player_get_state(void);

const char *player_state_name(player_state_t state);

#ifdef __cplusplus
}
#endif
