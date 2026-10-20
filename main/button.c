#include "button.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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

struct button_dev {
    gpio_num_t gpio;
    void (*on_short_press)(void *ctx);
    void (*on_long_press)(void *ctx);
    void *ctx;
    uint32_t long_press_ms;
    uint32_t long_repeat_ms;
};

static void button_task(void *arg)
{
    button_handle_t btn = arg;
    bool pressed = false;
    bool long_fired = false;
    int stable_count = 0;
    int last_raw = gpio_get_level(btn->gpio);
    int64_t next_long_us = 0;

    while (1) {
        int raw = gpio_get_level(btn->gpio);

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
                    next_long_us = esp_timer_get_time() +
                                   (int64_t)btn->long_press_ms * 1000;
                } else if (!now_pressed && pressed) {
                    pressed = false;
                    if (!long_fired && btn->on_short_press) {
                        ESP_LOGD(TAG, "GPIO%d short press", btn->gpio);
                        btn->on_short_press(btn->ctx);
                    }
                }
            }
        }

        /* 按住达到阈值立即触发长按；配置了连发间隔则持续触发，松开后不再触发短按 */
        if (pressed && btn->on_long_press) {
            int64_t now = esp_timer_get_time();
            if (!long_fired && now >= next_long_us) {
                long_fired = true;
                ESP_LOGD(TAG, "GPIO%d long press", btn->gpio);
                btn->on_long_press(btn->ctx);
                if (btn->long_repeat_ms > 0) {
                    next_long_us = now + (int64_t)btn->long_repeat_ms * 1000;
                }
            } else if (long_fired && btn->long_repeat_ms > 0 && now >= next_long_us) {
                ESP_LOGD(TAG, "GPIO%d long press repeat", btn->gpio);
                btn->on_long_press(btn->ctx);
                next_long_us = now + (int64_t)btn->long_repeat_ms * 1000;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_create(const button_config_t *cfg, button_handle_t *out)
{
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    button_handle_t btn = calloc(1, sizeof(*btn));
    if (btn == NULL) {
        return ESP_ERR_NO_MEM;
    }

    btn->gpio = cfg->gpio;
    btn->on_short_press = cfg->on_short_press;
    btn->on_long_press = cfg->on_long_press;
    btn->ctx = cfg->ctx;
    btn->long_press_ms = cfg->long_press_ms ? cfg->long_press_ms : BUTTON_LONG_PRESS_MS;
    btn->long_repeat_ms = cfg->long_repeat_ms;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        free(btn);
        return err;
    }

    if (xTaskCreate(button_task, "button", 2048, btn, 10, NULL) != pdPASS) {
        free(btn);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "button on GPIO%d (long: %u ms, repeat: %u ms)",
             cfg->gpio, btn->long_press_ms, btn->long_repeat_ms);
    *out = btn;
    return ESP_OK;
}
