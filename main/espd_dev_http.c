/*
 * SoftAP device console — HTTPS :443 only (UI + WSS /ws).
 */

#include "espd_dev_http.h"

#if CONFIG_ESPD_WIFI_AP_SYNC

#include "espd_dev_wifi.h"

#include <esp_https_server.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
extern const unsigned char servercert_end[] asm("_binary_servercert_pem_end");
extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
extern const unsigned char prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");

static const char *TAG = "espd_dev_http";
static httpd_handle_t s_httpd;

static esp_err_t devconsole_send_file(httpd_req_t *req, const char *start, const char *end,
        const char *content_type)
{
    const size_t len = (size_t)(end - start);
    if (!start || end <= start) {
        ESP_LOGE(TAG, "embedded asset missing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "asset missing");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, len);
}

static esp_err_t devconsole_root_get(httpd_req_t *req)
{
    printf("wifi: GET %s\n", req->uri);
    return devconsole_send_file(req, index_html_start, index_html_end,
            "text/html; charset=utf-8");
}

static esp_err_t devconsole_app_js_get(httpd_req_t *req)
{
    return devconsole_send_file(req, app_js_start, app_js_end,
            "application/javascript; charset=utf-8");
}

static esp_err_t devconsole_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t devconsole_ws(httpd_req_t *req)
{
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;

    if (!espd_dev_wifi_client_active()) {
        espd_dev_wifi_session_begin_ws(req);
    }
    httpd_sess_update_lru_counter(req->handle, httpd_req_to_sockfd(req));

    memset(&ws_pkt, 0, sizeof(ws_pkt));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
        return ret;

    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf)
            return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK && ws_pkt.len > 0)
            espd_dev_wifi_rx_feed(buf, ws_pkt.len);
        httpd_sess_update_lru_counter(req->handle, httpd_req_to_sockfd(req));
        free(buf);
        if (ret != ESP_OK)
            return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
        espd_dev_wifi_session_end_ws(req);

    return ESP_OK;
}

static void devconsole_register_handlers(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = devconsole_root_get,
    };
    httpd_uri_t index_html = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = devconsole_root_get,
    };
    httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = devconsole_app_js_get,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = devconsole_favicon_get,
    };
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = devconsole_ws,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &index_html);
    httpd_register_uri_handler(server, &app_js);
    httpd_register_uri_handler(server, &favicon);
    httpd_register_uri_handler(server, &ws);
}

void espd_dev_http_init(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = {0};

    if (s_httpd)
        return;

    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK)
        printf("wifi: AP netif " IPSTR "\n", IP2STR(&ip.ip));

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    conf.servercert = servercert_start;
    conf.servercert_len = (size_t)(servercert_end - servercert_start);
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = (size_t)(prvtkey_pem_end - prvtkey_pem_start);
    /* No cacert_pem — browser-friendly (no client certificate request). */
    conf.tls_version = ESP_TLS_VER_TLS_1_2;
    conf.httpd.server_port = 443;
    conf.httpd.lru_purge_enable = true;
    conf.httpd.max_open_sockets = 7;
    conf.httpd.stack_size = 20480;
    /* Client may go silent while we commit a large PUT (flash write). */
    conf.httpd.recv_wait_timeout = 180;
    conf.httpd.keep_alive_enable = true;
    conf.httpd.keep_alive_idle = 30;
    conf.httpd.keep_alive_interval = 10;
    conf.httpd.keep_alive_count = 4;

    esp_err_t err = httpd_ssl_start(&s_httpd, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "https start failed: %s", esp_err_to_name(err));
        printf("wifi: device console https start failed: %s\n", esp_err_to_name(err));
        return;
    }
    devconsole_register_handlers(s_httpd);

    ESP_LOGI(TAG, "device console https://192.168.4.1/");
    printf("wifi: device console https://192.168.4.1/\n");
}

#endif /* CONFIG_ESPD_WIFI_AP_SYNC */
