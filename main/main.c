#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button.h"
#include "player.h"
#include "scanner.h"
#include "status_led.h"
#include "wifi_prov.h"

/* 播放控制按键：GPIO4，低电平按下 */
#define BUTTON_GPIO GPIO_NUM_4

/* 音量按键：GPIO5，短按音量减，长按音量加 */
#define VOLUME_BUTTON_GPIO GPIO_NUM_5
#define VOLUME_LONG_REPEAT_MS 200

/* BOOT 按键：GPIO0，运行中长按清除 WiFi 配网信息并重启 */
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_RESET_HOLD_MS 3000

/* 扫码枪：UART1 RX，扫码枪 TX 接此引脚 */
#define SCANNER_RX_GPIO GPIO_NUM_18
#define SCANNER_BAUD_RATE 115200

static const char *TAG = "app";

static void button_short_press(void *ctx)
{
    (void)ctx;
    player_toggle_play_pause();
}

static void button_long_press(void *ctx)
{
    (void)ctx;
    player_next();
}

static void volume_short_press(void *ctx)
{
    (void)ctx;
    player_volume_down();
}

static void volume_long_press(void *ctx)
{
    (void)ctx;
    player_volume_up();
}

static void boot_long_press(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "BOOT long press: erasing WiFi credentials");
    esp_err_t err = wifi_prov_erase_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase WiFi credentials failed: %s", esp_err_to_name(err));
    }
    status_led_set_mode(STATUS_LED_ERROR);

    /*
     * GPIO0 是下载模式 strapping 引脚：如果在仍按住 BOOT 时重启，芯片会进入
     * 串口下载模式而不是运行应用。因此等用户松开后再重启。
     */
    ESP_LOGW(TAG, "release BOOT to restart into provisioning mode");
    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void app_main(void)
{
    /* RGB 上电自检（红、绿、蓝、白各 200 ms），随后由状态灯任务接管 */
    ESP_ERROR_CHECK(status_led_init());

    /* 短按：播放/暂停；长按：下一首 */
    ESP_ERROR_CHECK(player_init());

    button_config_t play_btn = {
        .gpio = BUTTON_GPIO,
        .on_short_press = button_short_press,
        .on_long_press = button_long_press,
    };
    button_handle_t play_btn_handle;
    ESP_ERROR_CHECK(button_create(&play_btn, &play_btn_handle));

    /* 短按：音量减 1 级；长按：音量加，每 200 ms 连发一次 */
    button_config_t volume_btn = {
        .gpio = VOLUME_BUTTON_GPIO,
        .on_short_press = volume_short_press,
        .on_long_press = volume_long_press,
        .long_repeat_ms = VOLUME_LONG_REPEAT_MS,
    };
    button_handle_t volume_btn_handle;
    ESP_ERROR_CHECK(button_create(&volume_btn, &volume_btn_handle));

    /* BOOT 长按 3 秒：清除配网信息，重启后进入配网热点 */
    button_config_t boot_btn = {
        .gpio = BOOT_BUTTON_GPIO,
        .on_long_press = boot_long_press,
        .long_press_ms = BOOT_RESET_HOLD_MS,
    };
    button_handle_t boot_btn_handle;
    ESP_ERROR_CHECK(button_create(&boot_btn, &boot_btn_handle));

    /* 扫码枪数据打印到日志控制台 */
    ESP_ERROR_CHECK(scanner_init(SCANNER_RX_GPIO, SCANNER_BAUD_RATE));

    /* WiFi：已保存凭证则直接联网，否则开启 ESP32-Config 配网热点 */
    ESP_ERROR_CHECK(wifi_prov_init());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
