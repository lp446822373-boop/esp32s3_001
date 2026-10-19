#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

#include "button.h"
#include "player.h"

/*
 * ESP32-S3-DevKitC-1 板载可寻址 RGB LED（WS2812）。
 * v1.0 版本由 GPIO48 驱动，v1.1 版本为 GPIO38。
 */
#define RGB_LED_GPIO 48
#define RGB_LED_COUNT 1

/* 播放控制按键：GPIO4，低电平按下 */
#define BUTTON_GPIO GPIO_NUM_4

static const char *TAG = "rgb";

static void rgb_set(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void rgb_init(led_strip_handle_t *strip)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, strip));
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", RGB_LED_GPIO);
}

static void button_short_press(void)
{
    player_toggle_play_pause();
}

static void button_long_press(void)
{
    player_next();
}

void app_main(void)
{
    led_strip_handle_t strip;
    rgb_init(&strip);

    /* 上电自检：红、绿、蓝、白各亮 200 ms */
    const uint8_t selftest[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
    };
    for (int i = 0; i < sizeof(selftest) / sizeof(selftest[0]); i++) {
        rgb_set(strip, selftest[i][0], selftest[i][1], selftest[i][2]);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    rgb_set(strip, 0, 0, 0);
    ESP_LOGI(TAG, "self-test done, LED off");

    /* 短按：播放/暂停；长按：下一首 */
    ESP_ERROR_CHECK(player_init());
    ESP_ERROR_CHECK(button_init(BUTTON_GPIO, button_short_press, button_long_press));

    /* 保持熄灭 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
