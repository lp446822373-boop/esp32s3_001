#include "scanner.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCANNER_UART_NUM UART_NUM_1
#define SCANNER_UART_BUF_SIZE 2048
#define SCANNER_FRAME_MAX 256
#define SCANNER_HEX_MAX 64
#define SCANNER_IDLE_MS 100

static const char *TAG = "scanner";

/*
 * 一次扫码只打印一行日志：文本（不可打印字符替换为 '.'）+ 十六进制。
 * 超过 SCANNER_HEX_MAX 字节时只打印前 SCANNER_HEX_MAX 字节的十六进制。
 */
static void scanner_report(const uint8_t *data, uint32_t len, bool truncated)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    static uint32_t scan_count = 0;

    char text[SCANNER_FRAME_MAX + 1];
    char hex[SCANNER_HEX_MAX * 3 + 4];
    uint32_t hex_len = len < SCANNER_HEX_MAX ? len : SCANNER_HEX_MAX;
    size_t pos = 0;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        text[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    }
    text[len] = '\0';

    for (uint32_t i = 0; i < hex_len; i++) {
        hex[pos++] = hex_digits[data[i] >> 4];
        hex[pos++] = hex_digits[data[i] & 0x0F];
        hex[pos++] = ' ';
    }
    if (len > hex_len) {
        hex[pos++] = '.';
        hex[pos++] = '.';
        hex[pos++] = '.';
    }
    hex[pos] = '\0';

    scan_count++;
    if (truncated) {
        ESP_LOGW(TAG, "scan #%u (%u bytes, truncated): %s | %s",
                 scan_count, len, text, hex);
    } else {
        ESP_LOGI(TAG, "scan #%u (%u bytes): %s | %s",
                 scan_count, len, text, hex);
    }
}

static void scanner_rx_task(void *arg)
{
    uint8_t frame[SCANNER_FRAME_MAX];
    uint32_t len = 0;
    bool truncated = false;

    while (1) {
        uint8_t byte;
        int n = uart_read_bytes(SCANNER_UART_NUM, &byte, 1,
                                pdMS_TO_TICKS(SCANNER_IDLE_MS));

        if (n == 1) {
            if (byte == '\r' || byte == '\n') {
                if (len > 0 || truncated) {
                    scanner_report(frame, len, truncated);
                    len = 0;
                    truncated = false;
                }
            } else if (len < sizeof(frame)) {
                frame[len++] = byte;
            } else {
                /* 超出单帧容量，继续消费直到帧结束符或空闲超时 */
                truncated = true;
            }
        } else if (n == 0) {
            /* 空闲超时：扫码枪不带结束符时，把已收数据作为一帧 */
            if (len > 0 || truncated) {
                scanner_report(frame, len, truncated);
                len = 0;
                truncated = false;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t scanner_init(gpio_num_t rx_gpio, uint32_t baud_rate)
{
    uart_config_t uart_cfg = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(SCANNER_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(SCANNER_UART_NUM, UART_PIN_NO_CHANGE, rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_driver_install(SCANNER_UART_NUM, SCANNER_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(scanner_rx_task, "scanner", 3072, NULL, 9, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d RX=GPIO%d @ %u 8N1", SCANNER_UART_NUM, rx_gpio,
             (unsigned)baud_rate);
    return ESP_OK;
}
