#include "display.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "display_font.h"
#include "player.h"
#include "scanner.h"
#include "terminal.h"
#include "wifi_prov.h"

/* GM009605V4：0.96 寸 128x64 I2C OLED，SSD1306，默认地址 0x3C */
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT CONFIG_CANTEEN_DISPLAY_HEIGHT
#define DISPLAY_FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

#define DISPLAY_I2C_SPEED_HZ (400 * 1000)
#define DISPLAY_PROBE_TIMEOUT_MS 50
#define DISPLAY_SELF_TEST_HOLD_MS 500

#define DISPLAY_LINE_HEIGHT 16
#define DISPLAY_REFRESH_MS 100
#define DISPLAY_SCROLL_STEP 2
#define DISPLAY_SCROLL_START_MS 1000

/* 音频模块每 2 秒被查询一次，连续多个周期无应答才判定异常 */
#define PLAYER_ONLINE_TIMEOUT_US (8 * 1000 * 1000LL)
/* 扫码枪只发不收，空闲时没有数据，60 秒内有过数据即视为正常 */
#define SCANNER_ACTIVE_TIMEOUT_US (60 * 1000 * 1000LL)
/* 上报请求最长 10 秒超时，处理中页面至少停留这么久 */
#define PROCESSING_HOLD_US (15 * 1000 * 1000LL)
/* 待确认结果最长跟踪 10 分钟，超时后不再提示 */
#define PENDING_HINT_TIMEOUT_US (10 * 60 * 1000 * 1000LL)

static const char *TAG = "display";

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static volatile bool s_ready;

/* 1bpp 帧缓冲：位 0 在左、按页排列；固定按 64 行分配，32 行屏只用前一半 */
static uint8_t s_fb[DISPLAY_WIDTH * 64 / 8];
static uint8_t s_shown_fb[DISPLAY_WIDTH * 64 / 8];

/* 显示任务状态 */
static int64_t s_display_start_us;
static uint32_t s_last_seq;
static terminal_snapshot_t s_result;
static int64_t s_result_until_us;
static int s_scroll_x;
static int64_t s_scroll_next_us;

/* RSSI 每秒采样一次 */
static int8_t s_rssi;
static bool s_rssi_valid;
static int64_t s_rssi_next_us;

#if CONFIG_CANTEEN_DISPLAY_ENABLE

/* ---------- I2C / 面板初始化 ---------- */

static esp_err_t display_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_CANTEEN_DISPLAY_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_CANTEEN_DISPLAY_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* 模块自带 4.7k 上拉，内部上拉只作兜底 */
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

/* 返回探测到的地址；0 表示总线上没有应答 */
static uint16_t display_probe_addr(void)
{
    static const uint16_t candidates[] = {
        (uint16_t)CONFIG_CANTEEN_DISPLAY_I2C_ADDR,
        0x3C,
        0x3D,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] == 0) {
            continue;
        }
        if (i2c_master_probe(s_bus, candidates[i], DISPLAY_PROBE_TIMEOUT_MS) == ESP_OK) {
            return candidates[i];
        }
    }

    /* 配置地址和常见地址都不应答时扫描一遍总线，方便排查接线和地址跳线 */
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 10) == ESP_OK) {
            ESP_LOGW(TAG, "I2C device found at 0x%02X", addr);
        }
    }

    return 0;
}

static esp_err_t display_panel_init(uint16_t addr)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = DISPLAY_I2C_SPEED_HZ,
        .control_phase_bytes = 1, /* 根据 SSD1306 数据手册 */
        .dc_bit_offset = 6,       /* 根据 SSD1306 数据手册 */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .transaction_timeout_ms = 1000,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &s_io), TAG,
                        "create panel io failed");

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {
        .height = DISPLAY_HEIGHT,
        .contrast = CONFIG_CANTEEN_DISPLAY_CONTRAST,
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, /* 模块没有 RST 脚 */
        .bits_per_pixel = 1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &dev_cfg, &s_panel), TAG,
                        "create panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                        "panel display on failed");
    return ESP_OK;
}

/* ---------- 帧缓冲绘制原语 ---------- */

static void fb_clear(void)
{
    memset(s_fb, 0x00, DISPLAY_FB_SIZE);
}

static void fb_fill(uint8_t value)
{
    memset(s_fb, value, DISPLAY_FB_SIZE);
}

static void fb_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }

    size_t idx = (size_t)(y / 8) * DISPLAY_WIDTH + (size_t)x;
    if (on) {
        s_fb[idx] |= (uint8_t)(1u << (y % 8));
    } else {
        s_fb[idx] &= (uint8_t)~(1u << (y % 8));
    }
}

static void fb_rect(int x, int y, int w, int h, bool on)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            fb_pixel(x + col, y + row, on);
        }
    }
}

static esp_err_t fb_flush(void)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_fb);
}

/* ---------- 文字与图标 ---------- */

static void draw_glyph(int x, int y, const uint8_t *glyph, int w, int h, bool on)
{
    int stride = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            if (glyph[row * stride + col / 8] & (0x80 >> (col % 8))) {
                fb_pixel(x + col, y + row, on);
            }
        }
    }
}

/* 字库缺字时画一个空心方框，避免显示乱码 */
static void draw_missing_glyph(int x, int y, int w, int h, bool on)
{
    for (int col = 1; col < w - 1; col++) {
        fb_pixel(x + col, y, on);
        fb_pixel(x + col, y + h - 1, on);
    }
    for (int row = 1; row < h - 1; row++) {
        fb_pixel(x, y + row, on);
        fb_pixel(x + w - 1, y + row, on);
    }
}

static int text_width(const char *s)
{
    int width = 0;
    const char *p = s;

    while (*p != '\0') {
        uint32_t cp;
        int n = display_utf8_decode(p, &cp);
        if (n == 0) {
            break;
        }
        if (cp < 0x20) {
            /* 控制字符不占位，也不画方框 */
            p += n;
            continue;
        }
        int gw;
        int gh;
        if (display_font_lookup(cp, &gw, &gh) == NULL) {
            gw = 16;
        }
        width += gw;
        p += n;
    }

    return width;
}

/**
 * @brief 绘制一行 UTF-8 文本
 *
 * max_px 大于 0 时按字符边界截断，保证不画出该宽度。
 * x 允许为负，用于长文本滚动。
 */
static void draw_text(int x, int y, const char *s, int max_px, bool on)
{
    if (s == NULL) {
        return;
    }

    int cx = x;
    const char *p = s;
    while (*p != '\0') {
        uint32_t cp;
        int n = display_utf8_decode(p, &cp);
        if (n == 0) {
            break;
        }
        if (cp < 0x20) {
            p += n;
            continue;
        }

        int gw;
        int gh;
        const uint8_t *glyph = display_font_lookup(cp, &gw, &gh);

        if (max_px > 0 && cx + gw > x + max_px) {
            break;
        }

        if (glyph != NULL) {
            draw_glyph(cx, y, glyph, gw, gh, on);
        } else {
            /* 字库缺字时画方框，提示需要补充字模 */
            draw_missing_glyph(cx, y, gw, gh, on);
        }

        cx += gw;
        p += n;
    }
}

static void draw_text_centered(int y, const char *s, bool on)
{
    if (s == NULL) {
        return;
    }
    int width = text_width(s);
    draw_text((DISPLAY_WIDTH - width) / 2, y, s, 0, on);
}

/* 4 格信号：达到的级别画实心竖条，未达到的只留底部一个点 */
static void draw_signal(int x, int y, int level)
{
    static const int heights[4] = {4, 7, 10, 13};

    for (int i = 0; i < 4; i++) {
        int bar_x = x + i * 5;
        int bar_h = heights[i];
        int bar_y = y + heights[3] - bar_h;
        if (i < level) {
            fb_rect(bar_x, bar_y, 3, bar_h, true);
        } else {
            fb_rect(bar_x, y + heights[3] - 1, 3, 1, true);
        }
    }
}

static int rssi_level(int8_t rssi)
{
    if (rssi >= -55) {
        return 4;
    }
    if (rssi >= -67) {
        return 3;
    }
    if (rssi >= -78) {
        return 2;
    }
    if (rssi >= -90) {
        return 1;
    }
    return 0;
}

/* ---------- 页面 ---------- */

static void render_main(void)
{
    int64_t now = esp_timer_get_time();
    fb_clear();

    /* 第 1 行：WiFi 状态 + 信号格 */
    wifi_prov_state_t state = wifi_prov_get_state();
    bool connected = wifi_prov_is_connected();
    const char *wifi_text;
    int level = 0;

    if (state == WIFI_PROV_STATE_PROVISIONING) {
        wifi_text = "WiFi 配网中";
    } else if (state == WIFI_PROV_STATE_CONNECTING) {
        wifi_text = "WiFi 连接中";
    } else if (state == WIFI_PROV_STATE_RUNNING) {
        if (connected) {
            wifi_text = "WiFi 已连接";
            if (now >= s_rssi_next_us) {
                s_rssi_valid = wifi_prov_get_rssi(&s_rssi);
                s_rssi_next_us = now + 1000 * 1000LL;
            }
            if (s_rssi_valid) {
                level = rssi_level(s_rssi);
            }
        } else {
            wifi_text = "WiFi 重连中";
        }
    } else {
        wifi_text = "WiFi 启动中";
    }
    draw_text(0, 0, wifi_text, DISPLAY_WIDTH - 32, true);
    draw_signal(DISPLAY_WIDTH - 24, 1, level);

    /* 第 2 行：扫码枪 */
    scanner_stats_t scanner;
    scanner_get_stats(&scanner);
    const char *scanner_text;
    if (!scanner.uart_ready) {
        scanner_text = "扫码枪 串口异常";
    } else if (scanner.last_rx_us == 0 ||
               now - scanner.last_rx_us > SCANNER_ACTIVE_TIMEOUT_US) {
        scanner_text = "扫码枪 待验证";
    } else {
        scanner_text = "扫码枪 正常";
    }
    draw_text(0, DISPLAY_LINE_HEIGHT, scanner_text, 0, true);

    /* 第 3 行：音频模块 */
    int64_t reply_us = player_get_last_reply_us();
    const char *audio_text;
    if (reply_us == 0) {
        audio_text = (now - s_display_start_us < PLAYER_ONLINE_TIMEOUT_US) ? "音频 检测中"
                                                                           : "音频 通信异常";
    } else if (now - reply_us > PLAYER_ONLINE_TIMEOUT_US) {
        audio_text = "音频 通信异常";
    } else {
        audio_text = "音频 正常";
    }
    draw_text(0, DISPLAY_LINE_HEIGHT * 2, audio_text, 0, true);

    /* 第 4 行：提示 */
    terminal_snapshot_t snapshot;
    const char *hint = "就绪，请扫码";
    if (terminal_get_snapshot(&snapshot) && snapshot.event == TERMINAL_EVENT_PENDING &&
        now - snapshot.updated_us < PENDING_HINT_TIMEOUT_US) {
        hint = "待手机确认…";
    }
    draw_text(0, DISPLAY_LINE_HEIGHT * 3, hint, 0, true);
}

static const char *event_title(terminal_event_t event)
{
    switch (event) {
    case TERMINAL_EVENT_PROCESSING:
        return "已扫码";
    case TERMINAL_EVENT_SUCCESS:
        return "扣款成功";
    case TERMINAL_EVENT_FAILED:
        return "扣款失败";
    case TERMINAL_EVENT_PENDING:
        return "待手机确认";
    case TERMINAL_EVENT_UNCONFIRMED:
        return "结果未确认";
    default:
        return "";
    }
}

static const char *result_scroll_text(const terminal_snapshot_t *snapshot)
{
    switch (snapshot->event) {
    case TERMINAL_EVENT_SUCCESS:
    case TERMINAL_EVENT_FAILED:
    case TERMINAL_EVENT_PENDING:
    case TERMINAL_EVENT_UNCONFIRMED:
        return snapshot->message[0] != '\0' ? snapshot->message : NULL;
    default:
        return NULL;
    }
}

static void format_amount(long long cents, char *buf, size_t size)
{
    if (cents < 0) {
        snprintf(buf, size, "--");
        return;
    }
    snprintf(buf, size, "%lld.%02lld 元", cents / 100, cents % 100);
}

static void render_result(const terminal_snapshot_t *snapshot, int scroll_x)
{
    char line[160];
    fb_clear();

    /* 标题条反色显示 */
    fb_rect(0, 0, DISPLAY_WIDTH, DISPLAY_LINE_HEIGHT, true);
    draw_text_centered(0, event_title(snapshot->event), false);

    switch (snapshot->event) {
    case TERMINAL_EVENT_PROCESSING:
        draw_text(0, DISPLAY_LINE_HEIGHT, snapshot->token_tail[0] != '\0' ? snapshot->token_tail : "--",
                  DISPLAY_WIDTH, true);
        draw_text(0, DISPLAY_LINE_HEIGHT * 2, "正在上报…", DISPLAY_WIDTH, true);
        break;

    case TERMINAL_EVENT_SUCCESS:
        format_amount(snapshot->amount_cents, line, sizeof(line));
        draw_text(0, DISPLAY_LINE_HEIGHT, line, DISPLAY_WIDTH, true);

        if (snapshot->employee_name[0] != '\0' && snapshot->meal_name[0] != '\0') {
            snprintf(line, sizeof(line), "%s  %s", snapshot->employee_name,
                     snapshot->meal_name);
        } else if (snapshot->employee_name[0] != '\0') {
            snprintf(line, sizeof(line), "%s", snapshot->employee_name);
        } else {
            snprintf(line, sizeof(line), "%s", snapshot->meal_name);
        }
        draw_text(0, DISPLAY_LINE_HEIGHT * 2, line, DISPLAY_WIDTH, true);

        if (snapshot->message[0] != '\0') {
            draw_text(-scroll_x, DISPLAY_LINE_HEIGHT * 3, snapshot->message, 0, true);
        } else {
            draw_text(0, DISPLAY_LINE_HEIGHT * 3, snapshot->transaction_no, DISPLAY_WIDTH, true);
        }
        break;

    case TERMINAL_EVENT_FAILED:
        if (snapshot->code[0] != '\0') {
            snprintf(line, sizeof(line), "code: %s", snapshot->code);
            draw_text(0, DISPLAY_LINE_HEIGHT, line, DISPLAY_WIDTH, true);
        } else if (snapshot->amount_cents >= 0) {
            format_amount(snapshot->amount_cents, line, sizeof(line));
            draw_text(0, DISPLAY_LINE_HEIGHT, line, DISPLAY_WIDTH, true);
        }
        draw_text(-scroll_x, DISPLAY_LINE_HEIGHT * 2,
                  snapshot->message[0] != '\0' ? snapshot->message : "支付失败", 0, true);
        break;

    case TERMINAL_EVENT_PENDING:
        format_amount(snapshot->amount_cents, line, sizeof(line));
        draw_text(0, DISPLAY_LINE_HEIGHT, line, DISPLAY_WIDTH, true);
        draw_text(-scroll_x, DISPLAY_LINE_HEIGHT * 2,
                  snapshot->message[0] != '\0' ? snapshot->message : "请在手机上确认", 0, true);
        break;

    case TERMINAL_EVENT_UNCONFIRMED:
    default:
        if (snapshot->http_status > 0) {
            snprintf(line, sizeof(line), "HTTP %d", snapshot->http_status);
            draw_text(0, DISPLAY_LINE_HEIGHT, line, DISPLAY_WIDTH, true);
        } else {
            draw_text(0, DISPLAY_LINE_HEIGHT, "网络异常", DISPLAY_WIDTH, true);
        }
        draw_text(-scroll_x, DISPLAY_LINE_HEIGHT * 2,
                  snapshot->message[0] != '\0' ? snapshot->message : "请以后台记录为准", 0, true);
        draw_text(0, DISPLAY_LINE_HEIGHT * 3, "请勿重复扫码", DISPLAY_WIDTH, true);
        break;
    }
}

/* ---------- 点亮自检 ---------- */

static void display_self_test(void)
{
    esp_err_t err;

    /* 1. 全黑：清掉上电随机内容 */
    fb_clear();
    err = fb_flush();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 2. 全白：验证驱动、电荷泵和对比度 */
    fb_fill(0xFF);
    if (err == ESP_OK) {
        err = fb_flush();
    }
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_SELF_TEST_HOLD_MS));

    /* 3. 横条：验证页寻址和列映射 */
    for (int page = 0; page < DISPLAY_HEIGHT / 8; page++) {
        memset(s_fb + (size_t)page * DISPLAY_WIDTH, (page & 1) ? 0xAA : 0x55,
               DISPLAY_WIDTH);
    }
    if (err == ESP_OK) {
        err = fb_flush();
    }
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_SELF_TEST_HOLD_MS));

    /* 4. 边框 + 左上/右下实心块：验证原点、方向和边界 */
    fb_clear();
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        fb_pixel(x, 0, true);
        fb_pixel(x, DISPLAY_HEIGHT - 1, true);
    }
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        fb_pixel(0, y, true);
        fb_pixel(DISPLAY_WIDTH - 1, y, true);
    }
    for (int y = 2; y < 10; y++) {
        for (int x = 2; x < 18; x++) {
            fb_pixel(x, y, true);
        }
    }
    for (int y = DISPLAY_HEIGHT - 10; y < DISPLAY_HEIGHT - 2; y++) {
        for (int x = DISPLAY_WIDTH - 10; x < DISPLAY_WIDTH - 2; x++) {
            fb_pixel(x, y, true);
        }
    }
    if (err == ESP_OK) {
        err = fb_flush();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* 5. 清屏，准备交给正式界面 */
    fb_clear();
    if (err == ESP_OK) {
        err = fb_flush();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "self test flush failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "self test done, OLED %dx%d ready", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

/* ---------- 显示任务 ---------- */

static void display_task(void *arg)
{
    (void)arg;

    s_display_start_us = esp_timer_get_time();
    display_self_test();

    memset(s_shown_fb, 0xFF, sizeof(s_shown_fb));

    while (1) {
        int64_t now = esp_timer_get_time();
        terminal_snapshot_t snapshot;

        if (terminal_get_snapshot(&snapshot) && snapshot.seq != s_last_seq) {
            /* 新的扫码事件：整屏显示结果 */
            s_last_seq = snapshot.seq;
            s_result = snapshot;
            s_scroll_x = 0;
            s_scroll_next_us = now + DISPLAY_SCROLL_START_MS * 1000LL;
            s_result_until_us =
                now + ((snapshot.event == TERMINAL_EVENT_PROCESSING)
                           ? PROCESSING_HOLD_US
                           : CONFIG_CANTEEN_DISPLAY_RESULT_HOLD_MS * 1000LL);

            render_result(&s_result, 0);
            fb_flush();
            memcpy(s_shown_fb, s_fb, DISPLAY_FB_SIZE);
        } else if (now < s_result_until_us) {
            /* 结果页停留期间让超长信息缓慢左移 */
            const char *text = result_scroll_text(&s_result);
            int width = (text != NULL) ? text_width(text) : 0;

            if (width > DISPLAY_WIDTH && now >= s_scroll_next_us) {
                int max_scroll = width - DISPLAY_WIDTH;
                if (s_scroll_x < max_scroll) {
                    s_scroll_x += DISPLAY_SCROLL_STEP;
                    if (s_scroll_x > max_scroll) {
                        s_scroll_x = max_scroll;
                    }
                    render_result(&s_result, s_scroll_x);
                    fb_flush();
                    memcpy(s_shown_fb, s_fb, DISPLAY_FB_SIZE);
                }
                s_scroll_next_us = now + CONFIG_CANTEEN_DISPLAY_SCROLL_MS * 1000LL;
            }
        } else {
            /* 常规页面：内容有变化才刷新 I2C */
            render_main();
            if (memcmp(s_fb, s_shown_fb, DISPLAY_FB_SIZE) != 0) {
                fb_flush();
                memcpy(s_shown_fb, s_fb, DISPLAY_FB_SIZE);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));
    }
}

esp_err_t display_init(void)
{
    esp_err_t err = display_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t addr = display_probe_addr();
    if (addr == 0) {
        ESP_LOGE(TAG, "no I2C device found, check VCC/GND/SDA/SCL wiring");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "OLED found at 0x%02X", addr);

    err = display_panel_init(addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(display_task, "display", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    return ESP_OK;
}

bool display_is_ready(void)
{
    return s_ready;
}

#else /* !CONFIG_CANTEEN_DISPLAY_ENABLE */

esp_err_t display_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool display_is_ready(void)
{
    return false;
}

#endif
