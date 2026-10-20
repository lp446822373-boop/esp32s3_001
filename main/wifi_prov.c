#include "wifi_prov.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "prov_web.h"
#include "status_led.h"

static const char *TAG = "wifi_prov";

/* ---------- 配网参数 ---------- */

#define PROV_AP_SSID "ESP32-Config"
#define PROV_AP_CHANNEL 6
#define PROV_AP_MAX_CONN 4
#define PROV_AP_IP_STR "192.168.4.1"

#define STA_CONNECT_TIMEOUT_MS 15000
#define PROV_SHUTDOWN_DELAY_MS 2000
#define RECONNECT_DELAY_MS 5000

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define SCAN_MAX_APS 20

/* ---------- NVS ---------- */

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

/* ---------- 事件位 ---------- */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define PROV_SUCCESS_BIT BIT2

typedef enum {
    ST_BOOT = 0,
    ST_CONNECTING,   /* 正在尝试连接路由器 */
    ST_PROVISIONING, /* 配网热点运行中 */
    ST_RUNNING,      /* 已联网，热点已关闭 */
} prov_state_t;

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static esp_timer_handle_t s_reconnect_timer;

static volatile prov_state_t s_state = ST_BOOT;
static volatile bool s_connected;
static volatile int s_last_reason;

static char s_cfg_ssid[33];
static char s_cfg_pass[65];

static volatile bool s_dns_run;
static TaskHandle_t s_dns_task;

static wifi_ap_record_t s_scan_records[SCAN_MAX_APS];
static char s_scan_json[3072];

/* ---------- 工具函数 ---------- */

static void copy_str(char *dst, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep != NULL) {
            size_t len = strlen(rep);
            if (out + len >= dst_size) {
                break;
            }
            memcpy(dst + out, rep, len);
            out += len;
        } else {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = (char)*p;
        }
    }
    dst[out] = '\0';
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        char tmp[8];
        const char *rep = NULL;
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
            break;
        }
        if (rep != NULL) {
            memcpy(dst + out, rep, len);
        } else {
            dst[out] = (char)*p;
        }
        out += len;
    }
    dst[out] = '\0';
}

static bool json_append(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
    if (*off >= size) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *off) {
        return false;
    }
    *off += (size_t)n;
    return true;
}

/* 解码 application/x-www-form-urlencoded，返回解码后的字节数，失败返回 -1 */
static int url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (const char *p = src; *p != '\0'; p++) {
        char c = *p;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && p[1] != '\0' && p[2] != '\0' &&
                   isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            c = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        if (c == '\0') {
            return -1; /* 不接受编码后的 NUL 字节 */
        }
        if (out + 1 >= dst_size) {
            return -1;
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
    return (int)out;
}

static esp_err_t get_form_value(const char *body, const char *key, char *val, size_t val_size)
{
    char raw[256];
    esp_err_t err = httpd_query_key_value(body, key, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if (url_decode(raw, val, val_size) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ---------- NVS 凭证 ---------- */

static esp_err_t nvs_load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pass_size;
    if (nvs_get_str(handle, WIFI_NVS_KEY_PASS, pass, &len) != ESP_OK) {
        pass[0] = '\0'; /* 开放网络没有密码 */
    }

    nvs_close(handle);
    return (ssid[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_prov_erase_credentials(void)
{
    /* BOOT 长按回调可能早于 wifi_prov_init 执行，这里保证 NVS 已初始化 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(handle, WIFI_NVS_KEY_SSID);
    nvs_erase_key(handle, WIFI_NVS_KEY_PASS);
    err = nvs_commit(handle);
    nvs_close(handle);

    s_cfg_ssid[0] = '\0';
    s_cfg_pass[0] = '\0';
    return err;
}

bool wifi_prov_has_credentials(void)
{
    char ssid[33];
    char pass[65];
    return nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;
}

bool wifi_prov_is_connected(void)
{
    return s_connected;
}

/* ---------- DNS 强制门户 ---------- */

static int build_dns_response(const uint8_t *query, int qlen, uint8_t *resp, int rsize)
{
    if (qlen < 12 || rsize < 12) {
        return 0;
    }

    /* 跳过 QNAME */
    int pos = 12;
    while (pos < qlen) {
        uint8_t label_len = query[pos];
        if (label_len == 0) {
            pos++;
            break;
        }
        if ((label_len & 0xC0) != 0) {
            return 0; /* 查询中不应出现压缩指针 */
        }
        pos += 1 + label_len;
    }
    if (pos + 4 > qlen) {
        return 0;
    }

    uint16_t qtype = ((uint16_t)query[pos] << 8) | query[pos + 1];
    int qend = pos + 4;
    int answer_len = (qtype == 1) ? 16 : 0; /* 只对 A 记录应答 */
    if (qend + answer_len > rsize) {
        return 0;
    }

    memcpy(resp, query, qend);
    resp[2] = 0x81; /* QR=1, RD=1 */
    resp[3] = 0x80; /* RA=1, rcode=0 */
    resp[4] = 0;
    resp[5] = 1; /* QDCOUNT */
    resp[6] = 0;
    resp[7] = answer_len ? 1 : 0; /* ANCOUNT */
    resp[8] = 0;
    resp[9] = 0; /* NSCOUNT */
    resp[10] = 0;
    resp[11] = 0; /* ARCOUNT */

    if (answer_len == 0) {
        return qend;
    }

    uint8_t *answer = resp + qend;
    answer[0] = 0xC0; /* 指向 offset 12 的域名 */
    answer[1] = 0x0C;
    answer[2] = 0x00;
    answer[3] = 0x01; /* TYPE A */
    answer[4] = 0x00;
    answer[5] = 0x01; /* CLASS IN */
    answer[6] = 0;
    answer[7] = 0;
    answer[8] = 0;
    answer[9] = 60; /* TTL */
    answer[10] = 0x00;
    answer[11] = 0x04; /* RDLENGTH */
    answer[12] = 192;
    answer[13] = 168;
    answer[14] = 4;
    answer[15] = 1;
    return qend + answer_len;
}

static void dns_server_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed: errno %d", errno);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);

    esp_netif_ip_info_t ip_info;
    if (s_ap_netif != NULL && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        addr.sin_addr.s_addr = ip_info.ip.addr;
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: errno %d", errno);
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t rx[DNS_MAX_PACKET];
    uint8_t tx[DNS_MAX_PACKET];
    ESP_LOGI(TAG, "captive-portal DNS listening on " PROV_AP_IP_STR ":53");

    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        int out_len = build_dns_response(rx, len, tx, sizeof(tx));
        if (out_len > 0) {
            sendto(sock, tx, out_len, 0, (struct sockaddr *)&from, from_len);
        }
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void)
{
    s_dns_run = true;
    if (xTaskCreate(dns_server_task, "prov_dns", 4096, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        s_dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void stop_dns_server(void)
{
    if (s_dns_task == NULL) {
        return;
    }
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_dns_task != NULL) {
        ESP_LOGW(TAG, "DNS task did not exit in time");
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}

/* ---------- WiFi 连接 ---------- */

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)RECONNECT_DELAY_MS * 1000);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_state == ST_RUNNING && !s_connected) {
        ESP_LOGI(TAG, "reconnecting to WiFi...");
        if (esp_wifi_connect() != ESP_OK) {
            schedule_reconnect();
        }
    }
}

/* 阻塞等待 STA 连接结果，成功返回 ESP_OK */
static esp_err_t sta_connect_blocking(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);

    wifi_config_t wifi_cfg = {0};
    /*
     * SSID / 密码允许恰好占满 32 / 64 字节，不能用 copy_str 截成 31 / 63 字节；
     * 结构体已清零，未占满的部分保持为 0。
     */
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len > sizeof(wifi_cfg.sta.ssid) || pass_len > sizeof(wifi_cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, pass, pass_len);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    s_last_reason = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if ((bits & WIFI_DISCONNECTED_BIT) == 0) {
        /* 超时仍未断开，主动取消连接，并等事件落定，避免影响下一次尝试 */
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_events, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(500));
    }
    return ESP_ERR_TIMEOUT;
}

static const char *state_name(prov_state_t state)
{
    switch (state) {
    case ST_BOOT: return "boot";
    case ST_CONNECTING: return "connecting";
    case ST_PROVISIONING: return "provisioning";
    case ST_RUNNING: return "running";
    default: return "unknown";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
        s_last_reason = event->reason;
        s_connected = false;
        ESP_LOGW(TAG, "STA disconnected (reason %d, state %s)", event->reason,
                 state_name(s_state));

        if (s_state == ST_CONNECTING) {
            xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        } else if (s_state == ST_RUNNING) {
            status_led_set_mode(STATUS_LED_RECONNECTING);
            schedule_reconnect();
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        if (event->esp_netif != s_sta_netif) {
            return;
        }
        s_connected = true;
        s_last_reason = 0;
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
        }
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_state == ST_CONNECTING || s_state == ST_RUNNING) {
            status_led_set_mode(STATUS_LED_CONNECTED);
        }
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "softAP started: %s", PROV_AP_SSID);
    }
}

/* ---------- HTTP 服务 ---------- */

static esp_err_t send_error_page(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PROV_ERR_HEAD);
    if (message != NULL && message[0] != '\0') {
        httpd_resp_sendstr_chunk(req, message);
    }
    httpd_resp_sendstr_chunk(req, PROV_ERR_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static void make_reason_message(int reason, char *buf, size_t size)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        snprintf(buf, size, "找不到该 WiFi。请确认名称正确、路由器已开启，并且是 2.4 GHz 频段。");
        break;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        snprintf(buf, size, "密码错误或加密方式不兼容，请检查后重试。");
        break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
        snprintf(buf, size, "认证失败，请检查 WiFi 密码。");
        break;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        snprintf(buf, size, "无法接入该路由器（可能连接数已满或信号太弱）。");
        break;
    case 0:
        snprintf(buf, size, "连接超时，请确认路由器已开启并靠近设备后重试。");
        break;
    default:
        snprintf(buf, size, "连接失败（原因码 %d），请重试。", reason);
        break;
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char escaped_ssid[256];
    html_escape(s_cfg_ssid, escaped_ssid, sizeof(escaped_ssid));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_sendstr_chunk(req, PROV_PAGE_HEAD);
    /*
     * 注意：空字符串会发送长度为 0 的分块，等同于结束 chunked 响应。
     * 首次配网时预填 SSID 为空，必须跳过，否则页面会在 value=" 处被截断。
     */
    if (err == ESP_OK && escaped_ssid[0] != '\0') {
        err = httpd_resp_sendstr_chunk(req, escaped_ssid);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, PROV_PAGE_TAIL);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, NULL);
    }
    return err;
}

static bool ssid_seen_before(const wifi_ap_record_t *records, uint16_t index)
{
    for (uint16_t i = 0; i < index; i++) {
        if (strcmp((const char *)records[i].ssid, (const char *)records[index].ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (s_state != ST_PROVISIONING) {
        return send_json_error(req, "409 Conflict", "not provisioning");
    }

    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return send_json_error(req, "503 Service Unavailable", "scan failed");
    }

    uint16_t num = SCAN_MAX_APS;
    err = esp_wifi_scan_get_ap_records(&num, s_scan_records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get scan records failed: %s", esp_err_to_name(err));
        return send_json_error(req, "503 Service Unavailable", "scan failed");
    }
    if (num > SCAN_MAX_APS) {
        num = SCAN_MAX_APS;
    }

    /* 按信号强度降序，便于同名 AP 保留最强的一个 */
    for (uint16_t i = 1; i < num; i++) {
        wifi_ap_record_t tmp = s_scan_records[i];
        int j = (int)i - 1;
        while (j >= 0 && s_scan_records[j].rssi < tmp.rssi) {
            s_scan_records[j + 1] = s_scan_records[j];
            j--;
        }
        s_scan_records[j + 1] = tmp;
    }

    size_t off = 0;
    s_scan_json[0] = '\0';
    json_append(s_scan_json, sizeof(s_scan_json), &off, "[");

    bool first = true;
    for (uint16_t i = 0; i < num; i++) {
        if (s_scan_records[i].ssid[0] == '\0' || ssid_seen_before(s_scan_records, i)) {
            continue;
        }
        /* 忽略自己的配网热点 */
        if (strcmp((const char *)s_scan_records[i].ssid, PROV_AP_SSID) == 0) {
            continue;
        }
        char escaped[200];
        json_escape((const char *)s_scan_records[i].ssid, escaped, sizeof(escaped));
        if (!json_append(s_scan_json, sizeof(s_scan_json), &off,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d}", first ? "" : ",", escaped,
                         s_scan_records[i].rssi)) {
            break;
        }
        first = false;
    }

    if (!json_append(s_scan_json, sizeof(s_scan_json), &off, "]")) {
        snprintf(s_scan_json, sizeof(s_scan_json), "[]");
    }

    ESP_LOGI(TAG, "scan returned %u AP(s)", (unsigned)num);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_scan_json);
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[512];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return send_error_page(req, "413 Payload Too Large", "提交的数据过大，请重试。");
    }

    size_t received = 0;
    int timeout_retries = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_retries > 3) {
                return ESP_FAIL;
            }
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        timeout_retries = 0;
        received += (size_t)ret;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    esp_err_t err = get_form_value(body, "ssid", ssid, sizeof(ssid));
    if (err != ESP_OK || ssid[0] == '\0') {
        return send_error_page(req, "400 Bad Request", "WiFi 名称无效，请重新输入。");
    }

    char pass[65] = {0};
    err = get_form_value(body, "pass", pass, sizeof(pass));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return send_error_page(req, "400 Bad Request", "WiFi 密码无效，请重新输入。");
    }

    size_t pass_len = strlen(pass);
    if (pass_len > 0 && pass_len < 8) {
        return send_error_page(req, "400 Bad Request",
                               "WiFi 密码至少 8 位；开放网络请留空。");
    }

    ESP_LOGI(TAG, "connecting to \"%s\"...", ssid);
    status_led_set_mode(STATUS_LED_CONNECTING);
    s_state = ST_CONNECTING;

    err = sta_connect_blocking(ssid, pass, STA_CONNECT_TIMEOUT_MS);
    if (err == ESP_OK) {
        esp_err_t save_err = nvs_save_credentials(ssid, pass);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "save credentials failed: %s", esp_err_to_name(save_err));
        }
        copy_str(s_cfg_ssid, sizeof(s_cfg_ssid), ssid);
        copy_str(s_cfg_pass, sizeof(s_cfg_pass), pass);

        status_led_set_mode(STATUS_LED_CONNECTED);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, PROV_OK_PAGE);
        xEventGroupSetBits(s_events, PROV_SUCCESS_BIT);
        ESP_LOGI(TAG, "provisioned to \"%s\"", ssid);
        return ESP_OK;
    }

    s_state = ST_PROVISIONING;
    status_led_set_mode(STATUS_LED_PROVISIONING);

    char message[160];
    make_reason_message(s_last_reason, message, sizeof(message));
    ESP_LOGW(TAG, "connect to \"%s\" failed: %s", ssid, message);
    return send_error_page(req, "200 OK", message);
}

static esp_err_t catch_all_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        /* 强制门户：任意域名/路径都跳转到配置页 */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" PROV_AP_IP_STR "/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }

    /*
     * 手机连上配网热点后，后台 App（微信 mmtls、各类上报 SDK）仍会向互联网
     * 发 POST 等请求，并被 DNS 劫持到这里。直接回 204，避免 httpd 反复打印
     * "Method not allowed" 告警。
     */
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL};
    httpd_uri_t uri_connect = {
        .uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler, .user_ctx = NULL};
    httpd_uri_t uri_catch_all = {
        .uri = "/*", .method = HTTP_ANY, .handler = catch_all_handler, .user_ctx = NULL};

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_connect);
    httpd_register_uri_handler(s_httpd, &uri_catch_all);
    return ESP_OK;
}

/* ---------- 配网流程 ---------- */

static esp_err_t start_provisioning(void)
{
    wifi_config_t ap_cfg = {0};
    size_t ssid_len = strlen(PROV_AP_SSID);
    memcpy(ap_cfg.ap.ssid, PROV_AP_SSID, ssid_len);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    ap_cfg.ap.channel = PROV_AP_CHANNEL;
    ap_cfg.ap.max_connection = PROV_AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_state = ST_PROVISIONING;
    status_led_set_mode(STATUS_LED_PROVISIONING);

    /*
     * 手机连上配网热点后，后台 App 会把 TLS/私有协议数据发到被 DNS 劫持的
     * 192.168.4.1:80，httpd 会打印 405/400 之类的客户端告警。配网期间只保留
     * httpd 的 ERROR 级别日志，避免串口被后台流量刷屏。
     */
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    err = start_http_server();
    if (err != ESP_OK) {
        return err;
    }
    if (start_dns_server() != ESP_OK) {
        ESP_LOGW(TAG, "captive-portal DNS failed to start, browse to http://" PROV_AP_IP_STR);
    }

    ESP_LOGI(TAG, "connect to open AP \"%s\" and open http://" PROV_AP_IP_STR,
             PROV_AP_SSID);
    return ESP_OK;
}

static void stop_provisioning(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    stop_dns_server();

    esp_log_level_set("httpd_parse", ESP_LOG_INFO);
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);

    esp_wifi_set_mode(WIFI_MODE_STA);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    ESP_LOGI(TAG, "softAP stopped");
}

static void wifi_prov_task(void *arg)
{
    (void)arg;

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_credentials =
        (nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK);

    if (have_credentials) {
        copy_str(s_cfg_ssid, sizeof(s_cfg_ssid), ssid);
        copy_str(s_cfg_pass, sizeof(s_cfg_pass), pass);
        ESP_LOGI(TAG, "found saved WiFi \"%s\", connecting...", ssid);

        s_state = ST_CONNECTING;
        status_led_set_mode(STATUS_LED_CONNECTING);
        esp_err_t err = sta_connect_blocking(ssid, pass, STA_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK) {
            s_state = ST_RUNNING;
            status_led_set_mode(STATUS_LED_CONNECTED);
            ESP_LOGI(TAG, "connected to \"%s\"", ssid);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "startup connect failed (reason %d), starting provisioning AP",
                 s_last_reason);
    } else {
        ESP_LOGI(TAG, "no saved WiFi credentials");
    }

    esp_err_t err = start_provisioning();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start provisioning failed: %s", esp_err_to_name(err));
        status_led_set_mode(STATUS_LED_ERROR);
        vTaskDelete(NULL);
        return;
    }

    /* 等待网页配网成功；HTTP 处理器完成连接后置位 */
    xEventGroupWaitBits(s_events, PROV_SUCCESS_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

    ESP_LOGI(TAG, "provisioning succeeded, closing softAP in %d ms", PROV_SHUTDOWN_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(PROV_SHUTDOWN_DELAY_MS));

    s_state = ST_RUNNING;
    status_led_set_mode(s_connected ? STATUS_LED_CONNECTED : STATUS_LED_RECONNECTING);
    stop_provisioning();

    if (!s_connected) {
        esp_wifi_connect();
        schedule_reconnect();
    }

    ESP_LOGI(TAG, "running in STA mode");
    vTaskDelete(NULL);
}

esp_err_t wifi_prov_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "failed to create default netifs");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(wifi_prov_task, "wifi_prov", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
