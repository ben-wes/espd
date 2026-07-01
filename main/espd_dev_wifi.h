#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if CONFIG_ESPD_WIFI_AP_SYNC

struct httpd_req;
typedef struct httpd_req httpd_req_t;

void espd_dev_wifi_init(void);
void espd_dev_wifi_stop_ap(void);

int espd_dev_wifi_read_hw(uint8_t *buf, size_t max);
/** Send sync/log bytes to the active Wi‑Fi client. Returns false if skipped (busy). */
bool espd_dev_wifi_write(const void *data, size_t len, bool wait);
bool espd_dev_wifi_client_active(void);
void espd_dev_wifi_notify_task(void);

void espd_dev_wifi_rx_feed(const void *data, size_t len);
void espd_dev_wifi_session_begin_ws(httpd_req_t *req);
void espd_dev_wifi_session_end_ws(httpd_req_t *req);
/** HTTP server closed a socket (LRU, timeout, client reset). */
void espd_dev_wifi_on_http_close(int sockfd);
void espd_dev_wifi_touch_session(void);

#endif
