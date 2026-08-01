/*
 * ESP-NOW core — init, peer table, async send/recv, completion delivery via
 * espd_now_poll() on the Pd/audio thread. Requires WiFi PHY up (wifi_prepare_phy).
 *
 * One global event handler (set by the Pd layer). SEND events carry the owner
 * pointer stashed at send time; RECV events carry owner=NULL and go to every
 * registered listener (every espdnow object hears every frame — the patch
 * routes by sender MAC if needed).
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define ESPD_NOW_MAX_PAYLOAD 250
#define ESPD_NOW_PENDING_DEPTH 8

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPD_NOW_EV_SEND_OK = 0,
    ESPD_NOW_EV_SEND_FAIL,
    ESPD_NOW_EV_RECV,
} espd_now_ev_type_t;

typedef struct {
    espd_now_ev_type_t type;
    void *owner;            /* sender object (SEND_*); NULL for RECV */
    uint8_t mac[6];         /* peer (SEND_*) or sender (RECV) */
    int8_t rssi;            /* RECV only */
    int len;                /* RECV payload length */
    uint8_t payload[ESPD_NOW_MAX_PAYLOAD]; /* RECV only */
    esp_err_t reason;       /* SEND_FAIL only */
} espd_now_event_t;

typedef void (*espd_now_event_cb)(const espd_now_event_t *ev);

void espd_now_init(void);           /* idempotent; call after wifi_prepare_phy */
bool espd_now_ready(void);

void espd_now_set_event_handler(espd_now_event_cb cb);

/* Peer table management. Broadcast peer (ff:ff:ff:ff:ff:ff) is always
 * unencrypted. encrypt=true uses the global PMK (espnow_pmk= in config.txt). */
esp_err_t espd_now_peer_add(const uint8_t mac[6], bool encrypt);
esp_err_t espd_now_peer_del(const uint8_t mac[6]);

/* Async send. owner is passed back in the SEND_* event. Returns ESP_OK if
 * queued; the actual result arrives via the event handler. */
esp_err_t espd_now_send(void *owner, const uint8_t mac[6],
    const uint8_t *data, int len);

/* Drain the event queue and dispatch to the handler. Call from Pd thread. */
void espd_now_poll(void);

#ifdef __cplusplus
}
#endif
