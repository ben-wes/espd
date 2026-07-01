/*
 * espd_dev sync over WiFi SoftAP — TCP :4499 (Python) and WebSocket /ws (browser).
 * Same STATUS/PUT/LIST/… semantics as USB serial (see espd_dev.c).
 */

#include "espd_dev_wifi.h"

#if CONFIG_ESPD_WIFI_AP_SYNC

#include "espd.h"
#include "espd_dev.h"
#include "espd_dev_http.h"
#include "wifi.h"

#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define ESPD_DEV_WIFI_PORT      4499
/* One PUT window (stop-and-wait); same as USB serial. */
#define ESPD_DEV_WIFI_RX_CAP    8192
#define ESPD_DEV_WIFI_STACK     4096

static const char *TAG = "espd_dev_wifi";

typedef enum {
    SESSION_NONE = 0,
    SESSION_TCP,
    SESSION_WS,
} session_kind_t;

static TaskHandle_t s_listen_task;
static TaskHandle_t s_client_task;
static int s_listen_fd = -1;
static SemaphoreHandle_t s_rx_mux;
static uint8_t s_rx_buf[ESPD_DEV_WIFI_RX_CAP];
static size_t s_rx_len;
static volatile bool s_client_active;
static session_kind_t s_session_kind;
static int s_tcp_fd = -1;
static httpd_handle_t s_ws_httpd;
static int s_ws_fd = -1;

static void espd_dev_wifi_clear_rx(void)
{
    if (s_rx_mux)
        xSemaphoreTake(s_rx_mux, portMAX_DELAY);
    s_rx_len = 0;
    if (s_rx_mux)
        xSemaphoreGive(s_rx_mux);
}

static void espd_dev_wifi_session_close(void)
{
    if (s_session_kind == SESSION_TCP && s_tcp_fd >= 0) {
        shutdown(s_tcp_fd, SHUT_RDWR);
        close(s_tcp_fd);
        s_tcp_fd = -1;
    }
    if (s_session_kind == SESSION_WS && s_ws_httpd && s_ws_fd >= 0) {
        httpd_sess_trigger_close(s_ws_httpd, s_ws_fd);
        s_ws_httpd = NULL;
        s_ws_fd = -1;
    }
    s_session_kind = SESSION_NONE;
    s_client_active = false;
    espd_dev_wifi_clear_rx();
    espd_dev_transport_reset();
}

static void espd_dev_wifi_close_tcp_client(void)
{
    espd_dev_wifi_session_close();
    s_client_task = NULL;
}

void espd_dev_wifi_rx_feed(const void *data, size_t len)
{
    const uint8_t *in = data;

    if (!data || len == 0 || !s_rx_mux)
        return;
    if (xSemaphoreTake(s_rx_mux, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    size_t space = ESPD_DEV_WIFI_RX_CAP - s_rx_len;
    if (len > space)
        len = space;
    if (len > 0) {
        memcpy(s_rx_buf + s_rx_len, in, len);
        s_rx_len += len;
    }
    xSemaphoreGive(s_rx_mux);
    espd_dev_wifi_notify_task();
}

static void espd_dev_wifi_client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t chunk[1024];

    s_tcp_fd = fd;
    s_session_kind = SESSION_TCP;
    s_client_active = true;
    espd_dev_wifi_clear_rx();
    espd_dev_transport_reset();
    ESP_LOGI(TAG, "TCP sync client connected");
    printf("sync: wifi TCP client connected\n");

    for (;;) {
        int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
            break;
        espd_dev_wifi_rx_feed(chunk, (size_t)n);
    }

    ESP_LOGI(TAG, "TCP sync client disconnected");
    printf("sync: wifi TCP client disconnected\n");
    espd_dev_wifi_close_tcp_client();
    vTaskDelete(NULL);
}

static void espd_dev_wifi_listen_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int on = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ESPD_DEV_WIFI_PORT);
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed errno=%d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on TCP %d", ESPD_DEV_WIFI_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(s_listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client < 0)
            continue;
        if (s_client_task || s_client_active) {
            espd_dev_wifi_session_close();
            if (s_client_task) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (xTaskCreate(espd_dev_wifi_client_task, "espd_wifi_cli",
                ESPD_DEV_WIFI_STACK, (void *)(intptr_t)client, 4, &s_client_task) != pdPASS) {
            close(client);
        }
    }
}

void espd_dev_wifi_init(void)
{
    if (s_listen_task)
        return;
    s_rx_mux = xSemaphoreCreateMutex();
    if (xTaskCreatePinnedToCore(espd_dev_wifi_listen_task, "espd_wifi_srv",
            ESPD_DEV_WIFI_STACK, NULL, 3, &s_listen_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "listen task create failed");
        s_listen_task = NULL;
    }
}

void espd_dev_wifi_stop_ap(void)
{
    wifi_stop_ap();
}

bool espd_dev_wifi_client_active(void)
{
    return s_client_active && s_session_kind != SESSION_NONE;
}

void espd_dev_wifi_notify_task(void)
{
    TaskHandle_t t = espd_dev_task_handle();
    if (t)
        xTaskNotifyGive(t);
}

int espd_dev_wifi_read_hw(uint8_t *buf, size_t max)
{
    int n = 0;

    if (!buf || max == 0 || !s_rx_mux)
        return 0;
    if (xSemaphoreTake(s_rx_mux, pdMS_TO_TICKS(0)) != pdTRUE)
        return 0;
    if (s_rx_len > 0) {
        if (max > s_rx_len)
            max = s_rx_len;
        memcpy(buf, s_rx_buf, max);
        memmove(s_rx_buf, s_rx_buf + max, s_rx_len - max);
        s_rx_len -= max;
        n = (int)max;
    }
    xSemaphoreGive(s_rx_mux);
    return n;
}

void espd_dev_wifi_touch_session(void)
{
    if (s_session_kind == SESSION_WS && s_ws_httpd && s_ws_fd >= 0)
        httpd_sess_update_lru_counter(s_ws_httpd, s_ws_fd);
}

void espd_dev_wifi_write(const void *data, size_t len)
{
    if (!espd_dev_wifi_client_active() || !data || len == 0)
        return;

    if (s_session_kind == SESSION_TCP && s_tcp_fd >= 0) {
        ssize_t sent = send(s_tcp_fd, data, len, 0);
        if (sent < 0)
            ESP_LOGW(TAG, "TCP send failed errno=%d", errno);
        return;
    }

    if (s_session_kind == SESSION_WS && s_ws_httpd && s_ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)data,
            .len = len,
        };
        esp_err_t err = httpd_ws_send_frame_async(s_ws_httpd, s_ws_fd, &ws_pkt);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "WS send failed %s", esp_err_to_name(err));
        else
            httpd_sess_update_lru_counter(s_ws_httpd, s_ws_fd);
    }
}

void espd_dev_wifi_session_begin_ws(httpd_req_t *req)
{
    if (!req)
        return;
    if (s_client_task || s_client_active) {
        espd_dev_wifi_session_close();
        if (s_client_task) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    s_ws_httpd = req->handle;
    s_ws_fd = httpd_req_to_sockfd(req);
    s_session_kind = SESSION_WS;
    s_client_active = true;
    espd_dev_wifi_clear_rx();
    espd_dev_transport_reset();
    ESP_LOGI(TAG, "WebSocket sync client connected");
    printf("sync: wifi WebSocket client connected\n");
}

void espd_dev_wifi_session_end_ws(httpd_req_t *req)
{
    (void)req;
    if (s_session_kind != SESSION_WS)
        return;
    ESP_LOGI(TAG, "WebSocket sync client disconnected");
    printf("sync: wifi WebSocket client disconnected\n");
    s_ws_httpd = NULL;
    s_ws_fd = -1;
    s_session_kind = SESSION_NONE;
    s_client_active = false;
    espd_dev_wifi_clear_rx();
    espd_dev_transport_reset();
}

#endif /* CONFIG_ESPD_WIFI_AP_SYNC */
