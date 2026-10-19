#include "player.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* UART0 默认引脚：TX = GPIO43，RX = GPIO44 */
#define PLAYER_UART_NUM UART_NUM_0
#define PLAYER_TX_GPIO 43
#define PLAYER_RX_GPIO 44
#define PLAYER_BAUD_RATE 115200

/* 协议帧：7E LEN CMD [PARAM...] SUM EF */
#define FRAME_START 0x7E
#define FRAME_END 0xEF

#define CMD_PLAY_INDEX 0xA0
#define CMD_PAUSE_RESUME 0xAA
#define CMD_NEXT 0xAC
#define CMD_QUERY_STATUS 0xC2

#define STATUS_PLAYING 0x01
#define STATUS_STOPPED 0x02
#define STATUS_PAUSED 0x03

/* 发送命令后的一小段时间内忽略状态回包，避免旧查询结果覆盖本地状态 */
#define STATUS_IGNORE_US (300 * 1000)

static const char *TAG = "player";

static volatile player_state_t s_state = PLAYER_STOPPED;
static volatile int64_t s_ignore_status_before_us = 0;
static SemaphoreHandle_t s_status_reply_sem;

const char *player_state_name(player_state_t state)
{
    switch (state) {
    case PLAYER_PLAYING:
        return "playing";
    case PLAYER_PAUSED:
        return "paused";
    case PLAYER_STOPPED:
    default:
        return "stopped";
    }
}

player_state_t player_get_state(void)
{
    return s_state;
}

static void send_cmd(uint8_t cmd, const uint8_t *params, uint8_t param_len)
{
    uint8_t frame[16];
    uint8_t len = param_len + 3; /* LEN 自身 + CMD + 参数 + SUM */
    uint8_t sum = len + cmd;
    size_t i = 0;

    frame[i++] = FRAME_START;
    frame[i++] = len;
    frame[i++] = cmd;
    for (uint8_t k = 0; k < param_len; k++) {
        frame[i++] = params[k];
        sum += params[k];
    }
    frame[i++] = sum;
    frame[i++] = FRAME_END;

    uart_write_bytes(PLAYER_UART_NUM, frame, i);
}

static void handle_frame(const uint8_t *body, uint8_t body_len)
{
    if (body_len < 2) {
        return;
    }

    uint8_t cmd = body[0];

    if (cmd == CMD_QUERY_STATUS) {
        /* 命令刚发出后收到的可能是旧状态，直接丢弃 */
        if (esp_timer_get_time() < s_ignore_status_before_us) {
            return;
        }

        switch (body[1]) {
        case STATUS_PLAYING:
            s_state = PLAYER_PLAYING;
            break;
        case STATUS_PAUSED:
            s_state = PLAYER_PAUSED;
            break;
        case STATUS_STOPPED:
        default:
            s_state = PLAYER_STOPPED;
            break;
        }
        xSemaphoreGive(s_status_reply_sem);
    } else {
        ESP_LOGD(TAG, "reply cmd=0x%02X", cmd);
    }
}

static void player_rx_task(void *arg)
{
    typedef enum {
        PARSE_IDLE,
        PARSE_LEN,
        PARSE_BODY,
        PARSE_END,
    } parse_state_t;

    parse_state_t parse = PARSE_IDLE;
    uint8_t len = 0;
    uint8_t body[36];
    uint8_t body_len = 0;
    uint8_t idx = 0;
    uint8_t byte;

    while (1) {
        if (uart_read_bytes(PLAYER_UART_NUM, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }

        switch (parse) {
        case PARSE_IDLE:
            if (byte == FRAME_START) {
                parse = PARSE_LEN;
            }
            break;

        case PARSE_LEN:
            /* LEN 至少为 3，且 LEN-1 字节（CMD..SUM）要能放进 body */
            if (byte < 3 || (uint8_t)(byte - 1) > sizeof(body)) {
                parse = (byte == FRAME_START) ? PARSE_LEN : PARSE_IDLE;
                break;
            }
            len = byte;
            body_len = len - 1;
            idx = 0;
            parse = PARSE_BODY;
            break;

        case PARSE_BODY:
            body[idx++] = byte;
            if (idx >= body_len) {
                uint8_t sum = len;
                for (uint8_t i = 0; i + 1 < body_len; i++) {
                    sum += body[i];
                }
                if (sum == body[body_len - 1]) {
                    handle_frame(body, body_len);
                }
                parse = PARSE_END;
            }
            break;

        case PARSE_END:
            parse = (byte == FRAME_END) ? PARSE_IDLE
                                        : (byte == FRAME_START ? PARSE_LEN : PARSE_IDLE);
            break;
        }

        /* 处理函数可能耗时，主动让出 CPU */
        if (parse == PARSE_IDLE) {
            taskYIELD();
        }
    }
}

static void player_poll_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        send_cmd(CMD_QUERY_STATUS, NULL, 0);
    }
}

esp_err_t player_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = PLAYER_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(PLAYER_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(PLAYER_UART_NUM, PLAYER_TX_GPIO, PLAYER_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_driver_install(PLAYER_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    s_status_reply_sem = xSemaphoreCreateBinary();
    if (s_status_reply_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(player_rx_task, "player_rx", 3072, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(player_poll_task, "player_poll", 2048, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* 上电查询一次模块状态（查询本身就是帧，无需忽略期） */
    send_cmd(CMD_QUERY_STATUS, NULL, 0);
    if (xSemaphoreTake(s_status_reply_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
        ESP_LOGI(TAG, "player connected, state: %s", player_state_name(s_state));
    } else {
        ESP_LOGW(TAG, "no reply from player, assume stopped");
        s_state = PLAYER_STOPPED;
    }

    return ESP_OK;
}

void player_toggle_play_pause(void)
{
    switch (s_state) {
    case PLAYER_PLAYING:
        s_ignore_status_before_us = esp_timer_get_time() + STATUS_IGNORE_US;
        send_cmd(CMD_PAUSE_RESUME, NULL, 0);
        s_state = PLAYER_PAUSED;
        ESP_LOGI(TAG, "pause");
        break;

    case PLAYER_PAUSED:
        s_ignore_status_before_us = esp_timer_get_time() + STATUS_IGNORE_US;
        send_cmd(CMD_PAUSE_RESUME, NULL, 0);
        s_state = PLAYER_PLAYING;
        ESP_LOGI(TAG, "resume");
        break;

    case PLAYER_STOPPED:
    default: {
        /* 停止状态下 AA 无效，直接指定播放第 1 首（曲目号高字节在前） */
        const uint8_t track[] = {0x00, 0x01};
        s_ignore_status_before_us = esp_timer_get_time() + STATUS_IGNORE_US;
        send_cmd(CMD_PLAY_INDEX, track, sizeof(track));
        s_state = PLAYER_PLAYING;
        ESP_LOGI(TAG, "play track 1");
        break;
    }
    }
}

void player_next(void)
{
    s_ignore_status_before_us = esp_timer_get_time() + STATUS_IGNORE_US;
    send_cmd(CMD_NEXT, NULL, 0);
    s_state = PLAYER_PLAYING;
    ESP_LOGI(TAG, "next");
}
