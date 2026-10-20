#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

/*
 * ESP32-S3-DevKitC-1 板载可寻址 RGB LED（WS2812）。
 * v1.0 版本由 GPIO48 驱动，v1.1 版本为 GPIO38。
 */
#define STATUS_LED_GPIO 48
#define STATUS_LED_COUNT 1
#define STATUS_LED_TICK_MS 100
#define STATUS_LED_BRIGHTNESS 64 /* 0~255，适当降低亮度避免刺眼 */

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;
static volatile status_led_mode_t s_mode = STATUS_LED_OFF;

static uint8_t s_last_r;
static uint8_t s_last_g;
static uint8_t s_last_b;

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (r == s_last_r && g == s_last_g && b == s_last_b) {
        return;
    }
    ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
    s_last_r = r;
    s_last_g = g;
    s_last_b = b;
}

static void led_self_test(void)
{
    const uint8_t colors[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
    };
    for (int i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        led_write(colors[i][0], colors[i][1], colors[i][2]);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    led_write(0, 0, 0);
    ESP_LOGI(TAG, "self-test done, LED off");
}

static void status_led_task(void *arg)
{
    (void)arg;

    status_led_mode_t last_mode = STATUS_LED_OFF;
    int elapsed = 0;
    bool on = false;

    while (1) {
        status_led_mode_t mode = s_mode;
        if (mode != last_mode) {
            last_mode = mode;
            elapsed = 0;
        }

        switch (mode) {
        case STATUS_LED_OFF:
            led_write(0, 0, 0);
            break;
        case STATUS_LED_PROVISIONING:
            /* 蓝色慢闪：500 ms 亮 / 500 ms 灭 */
            on = ((elapsed / 5) % 2) == 0;
            led_write(0, 0, on ? STATUS_LED_BRIGHTNESS : 0);
            break;
        case STATUS_LED_CONNECTING:
            led_write(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS / 3, 0);
            break;
        case STATUS_LED_CONNECTED:
            /* 绿色亮 1.5 s 后熄灭，避免夜间常亮 */
            led_write(0, elapsed <= 15 ? STATUS_LED_BRIGHTNESS : 0, 0);
            break;
        case STATUS_LED_RECONNECTING:
            /* 橙色慢闪 */
            on = ((elapsed / 5) % 2) == 0;
            led_write(on ? STATUS_LED_BRIGHTNESS : 0,
                      on ? STATUS_LED_BRIGHTNESS / 4 : 0, 0);
            break;
        case STATUS_LED_ERROR:
            /* 红色快闪 */
            on = (elapsed % 2) == 0;
            led_write(on ? STATUS_LED_BRIGHTNESS : 0, 0, 0);
            break;
        default:
            led_write(0, 0, 0);
            break;
        }

        elapsed++;
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TICK_MS));
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init LED strip: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", STATUS_LED_GPIO);

    led_self_test();

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void status_led_set_mode(status_led_mode_t mode)
{
    s_mode = mode;
}
