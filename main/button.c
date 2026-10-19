#include "button.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 20
#define BUTTON_LONG_PRESS_MS 800

#define DEBOUNCE_SAMPLES (BUTTON_DEBOUNCE_MS / BUTTON_POLL_MS)
#define BUTTON_ACTIVE_LEVEL 0 /* 内部上拉，按下为低电平 */

static const char *TAG = "button";

static gpio_num_t s_gpio;
static void (*s_on_short_press)(void);
static void (*s_on_long_press)(void);

static void button_task(void *arg)
{
    bool pressed = false;
    bool long_fired = false;
    int stable_count = 0;
    int last_raw = gpio_get_level(s_gpio);
    int64_t press_start_us = 0;

    while (1) {
        int raw = gpio_get_level(s_gpio);

        if (raw != last_raw) {
            last_raw = raw;
            stable_count = 0;
        } else if (stable_count < DEBOUNCE_SAMPLES) {
            stable_count++;
            if (stable_count == DEBOUNCE_SAMPLES) {
                bool now_pressed = (raw == BUTTON_ACTIVE_LEVEL);

                if (now_pressed && !pressed) {
                    pressed = true;
                    long_fired = false;
                    press_start_us = esp_timer_get_time();
                } else if (!now_pressed && pressed) {
                    pressed = false;
                    if (!long_fired && s_on_short_press) {
                        s_on_short_press();
                    }
                }
            }
        }

        /* 按住达到阈值立即触发一次长按，松开后不再触发短按 */
        if (pressed && !long_fired &&
            esp_timer_get_time() - press_start_us >= (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
            long_fired = true;
            ESP_LOGD(TAG, "long press");
            if (s_on_long_press) {
                s_on_long_press();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_init(gpio_num_t gpio,
                      void (*on_short_press)(void),
                      void (*on_long_press)(void))
{
    s_gpio = gpio;
    s_on_short_press = on_short_press;
    s_on_long_press = on_long_press;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(button_task, "button", 2048, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "button on GPIO%d (short: play/pause, long: next)", gpio);
    return ESP_OK;
}
