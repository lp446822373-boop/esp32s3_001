#include "terminal.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "player.h"
#include "status_led.h"
#include "terminal_config.h"
#include "wifi_prov.h"

#define TERMINAL_SCAN_PATH "/api/v1/terminal/scan"
#define TERMINAL_HEARTBEAT_PATH "/api/v1/terminal/heartbeat"
#define TERMINAL_PENDING_PATH "/api/v1/terminal/pending/"

#define TERMINAL_TOKEN_MAX 128
#define TERMINAL_QUEUE_DEPTH 4
#define TERMINAL_RESP_MAX 1024
#define TERMINAL_PENDING_ID_MAX 64
#define TERMINAL_PENDING_MAX 4

#define TERMINAL_SCAN_TIMEOUT_MS 10000
#define TERMINAL_HEARTBEAT_TIMEOUT_MS 5000
#define TERMINAL_HEARTBEAT_INTERVAL_MS 10000
#define TERMINAL_HEARTBEAT_RETRY_MS 5000

/* 待确认请求：文档建议每 2 秒查询一次；服务端无最终结果时按超时放弃跟踪 */
#define TERMINAL_PENDING_QUERY_INTERVAL_MS 2000
#define TERMINAL_PENDING_QUERY_TIMEOUT_MS 5000
#define TERMINAL_PENDING_MAX_DURATION_US (10 * 60 * 1000 * 1000LL)

#define TERMINAL_SCAN_TASK_STACK 8192
#define TERMINAL_HEARTBEAT_TASK_STACK 8192
#define TERMINAL_PENDING_TASK_STACK 8192
#define TERMINAL_SCAN_TASK_PRIO 6
#define TERMINAL_HEARTBEAT_TASK_PRIO 5
#define TERMINAL_PENDING_TASK_PRIO 4

/* Kconfig 未生成时保持静默，方便在旧配置下编译 */
#ifndef CONFIG_CANTEEN_VOICE_TRACK_SUCCESS
#define CONFIG_CANTEEN_VOICE_TRACK_SUCCESS 0
#endif
#ifndef CONFIG_CANTEEN_VOICE_TRACK_FAILED
#define CONFIG_CANTEEN_VOICE_TRACK_FAILED 0
#endif
#ifndef CONFIG_CANTEEN_VOICE_TRACK_PENDING
#define CONFIG_CANTEEN_VOICE_TRACK_PENDING 0
#endif
#ifndef CONFIG_CANTEEN_VOICE_TRACK_UNCONFIRMED
#define CONFIG_CANTEEN_VOICE_TRACK_UNCONFIRMED 0
#endif

static const char *TAG = "terminal";

typedef enum {
    TERMINAL_RESULT_SUCCESS = 0,
    TERMINAL_RESULT_FAILED,
    TERMINAL_RESULT_PENDING,
    TERMINAL_RESULT_UNCONFIRMED,
} terminal_result_t;

typedef struct {
    uint16_t len;
    char token[TERMINAL_TOKEN_MAX + 1];
} scan_item_t;

typedef struct {
    char data[TERMINAL_RESP_MAX];
    size_t len;
    bool overflow;
} http_resp_t;

typedef struct {
    bool active;
    int64_t started_us;
    char id[TERMINAL_PENDING_ID_MAX + 1];
} pending_item_t;

static QueueHandle_t s_scan_queue;
static long long s_last_transaction_id = -1;
static portMUX_TYPE s_txid_lock = portMUX_INITIALIZER_UNLOCKED;

static pending_item_t s_pending_items[TERMINAL_PENDING_MAX];
static SemaphoreHandle_t s_pending_lock;

/* 最近一次扫码事件快照：上报任务写、显示任务读 */
static terminal_snapshot_t s_snapshot;
static uint32_t s_snapshot_seq;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

/* 按 UTF-8 字符边界截断，避免显示屏把汉字画成半个字符 */
static void copy_utf8(char *dst, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void snapshot_event(terminal_event_t event, int http_status, const char *code,
                           const char *message, const char *employee_name,
                           const char *meal_name, const char *transaction_no,
                           long long amount_cents, const char *token_tail)
{
    terminal_snapshot_t snap = {
        .event = event,
        .http_status = http_status,
        .amount_cents = amount_cents,
    };
    copy_utf8(snap.code, sizeof(snap.code), code);
    copy_utf8(snap.message, sizeof(snap.message), message);
    copy_utf8(snap.employee_name, sizeof(snap.employee_name), employee_name);
    copy_utf8(snap.meal_name, sizeof(snap.meal_name), meal_name);
    copy_utf8(snap.transaction_no, sizeof(snap.transaction_no), transaction_no);
    copy_utf8(snap.token_tail, sizeof(snap.token_tail), token_tail);

    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = snap;
    s_snapshot.seq = ++s_snapshot_seq;
    s_snapshot.updated_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_snapshot_lock);
}

bool terminal_get_snapshot(terminal_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);

    return out->event != TERMINAL_EVENT_NONE;
}

static void terminal_feedback(terminal_result_t result, bool play_voice)
{
    switch (result) {
    case TERMINAL_RESULT_SUCCESS:
        status_led_show_result(STATUS_LED_RESULT_SUCCESS);
        if (play_voice) {
            player_play_track(CONFIG_CANTEEN_VOICE_TRACK_SUCCESS);
        }
        break;
    case TERMINAL_RESULT_FAILED:
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        if (play_voice) {
            player_play_track(CONFIG_CANTEEN_VOICE_TRACK_FAILED);
        }
        break;
    case TERMINAL_RESULT_PENDING:
        status_led_show_result(STATUS_LED_RESULT_PENDING);
        if (play_voice) {
            player_play_track(CONFIG_CANTEEN_VOICE_TRACK_PENDING);
        }
        break;
    case TERMINAL_RESULT_UNCONFIRMED:
    default:
        status_led_show_result(STATUS_LED_RESULT_UNCONFIRMED);
        if (play_voice) {
            player_play_track(CONFIG_CANTEEN_VOICE_TRACK_UNCONFIRMED);
        }
        break;
    }
}

/* 只转义 JSON 字符串必需的字符；token 正常情况下是 ASCII */
static bool json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        const char *rep = NULL;
        char tmp[8];
        if (*p == '"') {
            rep = "\\\"";
        } else if (*p == '\\') {
            rep = "\\\\";
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            rep = tmp;
        }

        size_t len = (rep != NULL) ? strlen(rep) : 1;
        if (out + len >= dst_size) {
            return false;
        }
        if (rep != NULL) {
            memcpy(dst + out, rep, len);
        } else {
            dst[out] = (char)*p;
        }
        out += len;
    }
    dst[out] = '\0';
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || resp == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t len = (size_t)evt->data_len;
    if (resp->len + len < sizeof(resp->data)) {
        memcpy(resp->data + resp->len, evt->data, len);
        resp->len += len;
        resp->data[resp->len] = '\0';
    } else {
        resp->overflow = true;
    }
    return ESP_OK;
}

/* 发送请求；perform 成功时 http_status 返回 HTTP 状态码 */
static esp_err_t terminal_http_request(esp_http_client_method_t method, const char *path,
                                       const char *body, int timeout_ms, int *http_status,
                                       http_resp_t *resp)
{
    terminal_config_t cfg;
    terminal_config_get(&cfg);
    if (!terminal_config_is_complete(&cfg)) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[TERMINAL_URL_MAX + 128];
    int n = snprintf(url, sizeof(url), "%s%s", cfg.base_url, path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char auth[TERMINAL_KEY_MAX + 8];
    n = snprintf(auth, sizeof(auth), "Bearer %s", cfg.key);
    if (n < 0 || (size_t)n >= sizeof(auth)) {
        return ESP_ERR_INVALID_SIZE;
    }

    resp->len = 0;
    resp->overflow = false;
    resp->data[0] = '\0';
    if (http_status != NULL) {
        *http_status = 0;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Authorization", auth);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (err == ESP_OK && body != NULL) {
        err = esp_http_client_set_post_field(client, body, (int)strlen(body));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err == ESP_OK && http_status != NULL) {
        *http_status = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t terminal_http_post(const char *path, const char *body, int timeout_ms,
                                    int *http_status, http_resp_t *resp)
{
    return terminal_http_request(HTTP_METHOD_POST, path, body, timeout_ms, http_status, resp);
}

static esp_err_t terminal_http_get(const char *path, int timeout_ms, int *http_status,
                                   http_resp_t *resp)
{
    return terminal_http_request(HTTP_METHOD_GET, path, NULL, timeout_ms, http_status, resp);
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

static long long json_int(const cJSON *root, const char *name, long long fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : fallback;
}

static bool json_true(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsTrue(item);
}

static const char *or_dash(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : "-";
}

static void pending_track(const char *id)
{
    size_t len = strlen(id);
    if (len > TERMINAL_PENDING_ID_MAX) {
        ESP_LOGW(TAG, "pending id is %u bytes (limit %d), final result polling disabled",
                 (unsigned)len, TERMINAL_PENDING_ID_MAX);
        return;
    }

    int slot = -1;
    int oldest = 0;

    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    for (int i = 0; i < TERMINAL_PENDING_MAX; i++) {
        if (!s_pending_items[i].active) {
            if (slot < 0) {
                slot = i;
            }
        } else if (strcmp(s_pending_items[i].id, id) == 0) {
            /* 重复扫码返回同一个待确认请求：不重复跟踪 */
            xSemaphoreGive(s_pending_lock);
            return;
        } else if (s_pending_items[i].started_us < s_pending_items[oldest].started_us) {
            oldest = i;
        }
    }

    if (slot < 0) {
        /* 队列满：放弃跟踪最久的一条，避免新的扫码拿不到最终结果 */
        slot = oldest;
        ESP_LOGW(TAG, "pending queue full, stop polling %s", s_pending_items[slot].id);
    }

    s_pending_items[slot].active = true;
    s_pending_items[slot].started_us = esp_timer_get_time();
    memcpy(s_pending_items[slot].id, id, len + 1);
    xSemaphoreGive(s_pending_lock);

    ESP_LOGI(TAG, "polling pending %s every %d ms", id, TERMINAL_PENDING_QUERY_INTERVAL_MS);
}

static void pending_untrack(const char *id)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    for (int i = 0; i < TERMINAL_PENDING_MAX; i++) {
        if (s_pending_items[i].active && strcmp(s_pending_items[i].id, id) == 0) {
            s_pending_items[i].active = false;
            break;
        }
    }
    xSemaphoreGive(s_pending_lock);
}

/* 查询单个待确认请求；拿到 SUCCESS/FAILED 后停止跟踪并播报最终结果 */
static void pending_query(const char *id)
{
    char path[TERMINAL_PENDING_ID_MAX + 48];
    int n = snprintf(path, sizeof(path), TERMINAL_PENDING_PATH "%s", id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        pending_untrack(id);
        return;
    }

    int http_status = 0;
    http_resp_t resp;
    esp_err_t err = terminal_http_get(path, TERMINAL_PENDING_QUERY_TIMEOUT_MS, &http_status, &resp);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "pending %s: query failed (%s), will retry", id, esp_err_to_name(err));
        return;
    }
    if (http_status != 200) {
        ESP_LOGW(TAG, "pending %s: HTTP %d, will retry", id, http_status);
        return;
    }

    cJSON *root = resp.data[0] != '\0' ? cJSON_Parse(resp.data) : NULL;
    if (root == NULL) {
        ESP_LOGW(TAG, "pending %s: invalid JSON response, will retry", id);
        return;
    }

    const char *status = json_string(root, "status");
    const char *code = json_string(root, "code");
    const char *message = json_string(root, "message");

    if (status != NULL && strcmp(status, "SUCCESS") == 0) {
        long long txid = json_int(root, "transaction_id", -1);
        ESP_LOGI(TAG,
                 "pending %s confirmed: SUCCESS code=%s amount=%lld tx=%lld employee=%s meal=%s "
                 "transaction_no=%s message=%s",
                 id, or_dash(code), json_int(root, "amount_cents", -1), txid,
                 or_dash(json_string(root, "employee_name")),
                 or_dash(json_string(root, "meal_name")),
                 or_dash(json_string(root, "transaction_no")), or_dash(message));

        if (txid >= 0) {
            portENTER_CRITICAL(&s_txid_lock);
            s_last_transaction_id = txid;
            portEXIT_CRITICAL(&s_txid_lock);
        }
        pending_untrack(id);
        snapshot_event(TERMINAL_EVENT_SUCCESS, http_status, code, message,
                       json_string(root, "employee_name"), json_string(root, "meal_name"),
                       json_string(root, "transaction_no"),
                       json_int(root, "amount_cents", -1), NULL);
        terminal_feedback(TERMINAL_RESULT_SUCCESS, true);
    } else if (status != NULL && strcmp(status, "FAILED") == 0) {
        ESP_LOGW(TAG, "pending %s finished: FAILED code=%s message=%s", id, or_dash(code),
                 or_dash(message));
        pending_untrack(id);
        snapshot_event(TERMINAL_EVENT_FAILED, http_status, code, message, NULL, NULL, NULL,
                       json_int(root, "amount_cents", -1), NULL);
        terminal_feedback(TERMINAL_RESULT_FAILED, true);
    } else if (status != NULL && strcmp(status, "PENDING") == 0) {
        ESP_LOGD(TAG, "pending %s still waiting for employee confirmation", id);
    } else {
        ESP_LOGW(TAG, "pending %s: unknown status=%s, will retry", id, or_dash(status));
    }

    cJSON_Delete(root);
}

static void terminal_pending_task(void *arg)
{
    (void)arg;

    char current[TERMINAL_PENDING_MAX][TERMINAL_PENDING_ID_MAX + 1];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TERMINAL_PENDING_QUERY_INTERVAL_MS));

        if (!wifi_prov_is_connected()) {
            continue;
        }
        terminal_config_t cfg;
        terminal_config_get(&cfg);
        if (!terminal_config_is_complete(&cfg)) {
            continue;
        }

        /* HTTP 请求不持锁：只把当前要查询的编号拷出来 */
        int count = 0;
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        for (int i = 0; i < TERMINAL_PENDING_MAX; i++) {
            if (!s_pending_items[i].active) {
                continue;
            }
            if (esp_timer_get_time() - s_pending_items[i].started_us >
                TERMINAL_PENDING_MAX_DURATION_US) {
                ESP_LOGW(TAG, "pending %s: no final result after %d s, stop polling (check phone)",
                         s_pending_items[i].id, TERMINAL_PENDING_MAX_DURATION_US / 1000000);
                s_pending_items[i].active = false;
                continue;
            }
            memcpy(current[count], s_pending_items[i].id, sizeof(current[count]));
            count++;
        }
        xSemaphoreGive(s_pending_lock);

        for (int i = 0; i < count; i++) {
            pending_query(current[i]);
        }
    }
}

static void handle_scan_response(int http_status, const http_resp_t *resp)
{
    cJSON *root = NULL;
    if (!resp->overflow && resp->data[0] != '\0') {
        root = cJSON_Parse(resp->data);
    }

    /* 必须先判断 HTTP 状态，再解析业务 status */
    if (http_status != 200) {
        const char *code = root != NULL ? json_string(root, "code") : NULL;
        const char *message = root != NULL ? json_string(root, "message") : NULL;
        if (http_status == 401) {
            ESP_LOGE(TAG, "scan rejected: HTTP 401, check terminal key or whether the terminal is disabled");
        } else {
            ESP_LOGW(TAG, "scan failed: HTTP %d code=%s message=%s", http_status,
                     or_dash(code), or_dash(message));
        }
        if (root == NULL) {
            ESP_LOGW(TAG, "scan error response is not valid JSON");
        }
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, http_status, code, message, NULL, NULL, NULL,
                       -1, NULL);
        terminal_feedback(TERMINAL_RESULT_UNCONFIRMED, true);
        cJSON_Delete(root);
        return;
    }

    if (root == NULL) {
        ESP_LOGW(TAG, "scan result unconfirmed: invalid or oversized JSON");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, http_status, NULL, "响应解析失败", NULL, NULL,
                       NULL, -1, NULL);
        terminal_feedback(TERMINAL_RESULT_UNCONFIRMED, true);
        return;
    }

    const char *status = json_string(root, "status");
    const char *code = json_string(root, "code");
    const char *message = json_string(root, "message");
    long long amount = json_int(root, "amount_cents", -1);

    if (status != NULL && strcmp(status, "SUCCESS") == 0) {
        long long txid = json_int(root, "transaction_id", -1);
        bool replayed = json_true(root, "replayed");
        bool duplicate_trigger = json_true(root, "duplicate_trigger");

        portENTER_CRITICAL(&s_txid_lock);
        bool duplicate = replayed || duplicate_trigger ||
                         (txid >= 0 && txid == s_last_transaction_id);
        if (txid >= 0) {
            s_last_transaction_id = txid;
        }
        portEXIT_CRITICAL(&s_txid_lock);

        ESP_LOGI(TAG,
                 "scan SUCCESS code=%s amount=%lld tx=%lld duplicate=%d employee=%s meal=%s "
                 "transaction_no=%s message=%s",
                 or_dash(code), amount, txid, duplicate ? 1 : 0,
                 or_dash(json_string(root, "employee_name")),
                 or_dash(json_string(root, "meal_name")),
                 or_dash(json_string(root, "transaction_no")), or_dash(message));
        snapshot_event(TERMINAL_EVENT_SUCCESS, http_status, code, message,
                       json_string(root, "employee_name"), json_string(root, "meal_name"),
                       json_string(root, "transaction_no"), amount, NULL);
        terminal_feedback(TERMINAL_RESULT_SUCCESS, !duplicate);
    } else if (status != NULL && strcmp(status, "FAILED") == 0) {
        ESP_LOGW(TAG, "scan FAILED code=%s message=%s", or_dash(code), or_dash(message));
        snapshot_event(TERMINAL_EVENT_FAILED, http_status, code, message, NULL, NULL, NULL, amount,
                       NULL);
        terminal_feedback(TERMINAL_RESULT_FAILED, true);
    } else if (status != NULL && strcmp(status, "PENDING") == 0) {
        const char *pending_id = json_string(root, "pending_id");
        ESP_LOGW(TAG,
                 "scan PENDING code=%s amount=%lld pending_id=%s expires_at=%s message=%s "
                 "(waiting for employee confirmation on phone)",
                 or_dash(code), amount, or_dash(pending_id),
                 or_dash(json_string(root, "expires_at")), or_dash(message));
        snapshot_event(TERMINAL_EVENT_PENDING, http_status, code, message, NULL, NULL, NULL, amount,
                       NULL);
        if (pending_id != NULL && pending_id[0] != '\0') {
            pending_track(pending_id);
        } else {
            ESP_LOGW(TAG, "PENDING without pending_id: cannot poll for the final result");
        }
        terminal_feedback(TERMINAL_RESULT_PENDING, true);
    } else {
        ESP_LOGW(TAG, "scan result unconfirmed: unknown status=%s code=%s",
                 or_dash(status), or_dash(code));
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, http_status, code, message, NULL, NULL, NULL,
                       amount, NULL);
        terminal_feedback(TERMINAL_RESULT_UNCONFIRMED, true);
    }

    cJSON_Delete(root);
}

static void terminal_process_scan(const char *token)
{
    terminal_config_t cfg;
    terminal_config_get(&cfg);
    if (!terminal_config_is_complete(&cfg)) {
        ESP_LOGW(TAG, "scan dropped: terminal not configured, set server URL and key in provisioning page");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "未配置服务端", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_UNCONFIRMED);
        return;
    }

    /* 离线扫码不缓存、不补扣 */
    if (!wifi_prov_is_connected()) {
        ESP_LOGW(TAG, "scan result unconfirmed: WiFi offline");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "WiFi 离线", NULL, NULL, NULL, -1,
                       NULL);
        terminal_feedback(TERMINAL_RESULT_UNCONFIRMED, true);
        return;
    }

    char escaped[TERMINAL_TOKEN_MAX * 2 + 1];
    if (!json_escape(token, escaped, sizeof(escaped))) {
        ESP_LOGW(TAG, "scan dropped: token escaping failed");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "请求构造失败", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        return;
    }

    char body[TERMINAL_TOKEN_MAX * 2 + 32];
    int n = snprintf(body, sizeof(body), "{\"token\":\"%s\"}", escaped);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGW(TAG, "scan dropped: request body too long");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "请求构造失败", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        return;
    }

    int http_status = 0;
    http_resp_t resp;
    esp_err_t err = terminal_http_post(TERMINAL_SCAN_PATH, body, TERMINAL_SCAN_TIMEOUT_MS,
                                       &http_status, &resp);
    if (err != ESP_OK) {
        /* POST 超时可能发生在服务端扣款之后：只提示结果未确认，不自动重发 */
        ESP_LOGW(TAG, "scan result unconfirmed: request failed (%s), do not retry automatically",
                 esp_err_to_name(err));
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "请求失败或超时", NULL, NULL, NULL, -1,
                       NULL);
        terminal_feedback(TERMINAL_RESULT_UNCONFIRMED, true);
        return;
    }

    handle_scan_response(http_status, &resp);
}

static void terminal_scan_task(void *arg)
{
    (void)arg;

    scan_item_t item;
    while (1) {
        if (xQueueReceive(s_scan_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        terminal_process_scan(item.token);
    }
}

void terminal_on_scan_frame(const uint8_t *data, size_t len, bool truncated, void *ctx)
{
    (void)ctx;

    if (truncated) {
        ESP_LOGW(TAG, "scan dropped: frame truncated");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "扫码内容过长", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        return;
    }
    if (len == 0) {
        return;
    }
    if (len > TERMINAL_TOKEN_MAX) {
        ESP_LOGW(TAG, "scan dropped: %u bytes exceeds %d-byte token limit",
                 (unsigned)len, TERMINAL_TOKEN_MAX);
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "扫码内容过长", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        return;
    }
    if (len < 4 || memcmp(data, "pmt_", 4) != 0) {
        ESP_LOGW(TAG, "scan dropped: not a pmt_ meal code");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "非就餐码", NULL, NULL, NULL, -1, NULL);
        status_led_show_result(STATUS_LED_RESULT_FAILED);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x20 || data[i] == 0x7F) {
            ESP_LOGW(TAG, "scan dropped: contains control character");
            snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "扫码内容异常", NULL, NULL, NULL,
                           -1, NULL);
            status_led_show_result(STATUS_LED_RESULT_FAILED);
            return;
        }
    }

    if (s_scan_queue == NULL) {
        ESP_LOGW(TAG, "scan dropped: terminal client not started");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "终端未启动", NULL, NULL, NULL, -1,
                       NULL);
        return;
    }

    scan_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.token, data, len);
    item.token[len] = '\0';

    if (xQueueSend(s_scan_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "scan dropped: queue full");
        snapshot_event(TERMINAL_EVENT_UNCONFIRMED, 0, NULL, "请求队列已满", NULL, NULL, NULL, -1,
                       NULL);
        status_led_show_result(STATUS_LED_RESULT_UNCONFIRMED);
        return;
    }

    /* 让屏幕立即显示"已扫码、正在上报"，显示就餐码后 12 位 */
    const char *tail = item.token;
    size_t tail_len = strlen(tail);
    if (tail_len > 12) {
        tail += tail_len - 12;
    }
    snapshot_event(TERMINAL_EVENT_PROCESSING, 0, NULL, NULL, NULL, NULL, NULL, -1, tail);
}

static esp_err_t terminal_send_heartbeat(int *http_status)
{
    char body[96];
    int n = snprintf(body, sizeof(body),
                     "{\"scanner_status\":\"READY\",\"voice_status\":\"%s\"}",
                     player_is_available() ? "READY" : "UNAVAILABLE");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    http_resp_t resp;
    esp_err_t err = terminal_http_post(TERMINAL_HEARTBEAT_PATH, body,
                                       TERMINAL_HEARTBEAT_TIMEOUT_MS, http_status, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (*http_status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = resp.data[0] != '\0' ? cJSON_Parse(resp.data) : NULL;
    const char *status = root != NULL ? json_string(root, "status") : NULL;
    bool ok = status != NULL && strcmp(status, "ok") == 0;
    cJSON_Delete(root);

    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void terminal_heartbeat_task(void *arg)
{
    (void)arg;

    bool was_connected = false;
    bool error_logged = false;
    int64_t next_send_us = 0;

    while (1) {
        if (!wifi_prov_is_connected()) {
            was_connected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        terminal_config_t cfg;
        terminal_config_get(&cfg);
        if (!terminal_config_is_complete(&cfg)) {
            if (!error_logged) {
                ESP_LOGW(TAG, "heartbeat skipped: terminal not configured");
                error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (!was_connected) {
            was_connected = true;
            next_send_us = 0;
            ESP_LOGI(TAG, "network up, sending heartbeat");
        }

        if (now >= next_send_us) {
            int http_status = 0;
            esp_err_t err = terminal_send_heartbeat(&http_status);
            if (err == ESP_OK) {
                if (error_logged) {
                    ESP_LOGI(TAG, "heartbeat recovered");
                }
                error_logged = false;
                next_send_us = esp_timer_get_time() + TERMINAL_HEARTBEAT_INTERVAL_MS * 1000LL;
            } else {
                if (!error_logged) {
                    if (http_status == 401) {
                        ESP_LOGE(TAG, "heartbeat rejected: HTTP 401, check terminal key or whether the terminal is disabled");
                    } else if (http_status != 0) {
                        ESP_LOGW(TAG, "heartbeat rejected: HTTP %d", http_status);
                    } else {
                        ESP_LOGW(TAG, "heartbeat failed: %s", esp_err_to_name(err));
                    }
                    error_logged = true;
                }
                next_send_us = esp_timer_get_time() + TERMINAL_HEARTBEAT_RETRY_MS * 1000LL;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t terminal_init(void)
{
    if (s_scan_queue == NULL) {
        s_scan_queue = xQueueCreate(TERMINAL_QUEUE_DEPTH, sizeof(scan_item_t));
        if (s_scan_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_pending_lock == NULL) {
        s_pending_lock = xSemaphoreCreateMutex();
        if (s_pending_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(terminal_scan_task, "term_scan", TERMINAL_SCAN_TASK_STACK, NULL,
                    TERMINAL_SCAN_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(terminal_heartbeat_task, "term_hb", TERMINAL_HEARTBEAT_TASK_STACK, NULL,
                    TERMINAL_HEARTBEAT_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(terminal_pending_task, "term_pending", TERMINAL_PENDING_TASK_STACK, NULL,
                    TERMINAL_PENDING_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "terminal client started");
    return ESP_OK;
}
