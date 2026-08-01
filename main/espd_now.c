/*
 * ESP-NOW core — init, peer table, async send/recv, completion delivery via
 * espd_now_poll() on the Pd/audio thread.
 *
 * WiFi PHY must be up (wifi_prepare_phy) before espd_now_init(). ESP-NOW shares
 * the WiFi radio; it does not require STA association.
 */

#include "espd_now.h"
#include "espd_config.h"
#include "espd_config_file.h"

#include <esp_log.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#ifdef ESPD_USE_ESPNOW

static const char *TAG = "espd_now";

#ifndef ESPD_NOW_QUEUE_DEPTH
#define ESPD_NOW_QUEUE_DEPTH 12
#endif

static int s_inited;
static QueueHandle_t s_ev_q;
static espd_now_event_cb s_cb;

/* Pending-send ring: maps send_cb's (mac, status) back to the owner pointer. */
typedef struct {
    void *owner;
    uint8_t mac[6];
} espd_now_pending_t;
static espd_now_pending_t s_pending[ESPD_NOW_PENDING_DEPTH];

static int pending_push(void *owner, const uint8_t mac[6])
{
    int i;
    for (i = 0; i < ESPD_NOW_PENDING_DEPTH; i++) {
        if (s_pending[i].owner == NULL) {
            s_pending[i].owner = owner;
            memcpy(s_pending[i].mac, mac, 6);
            return 1;
        }
    }
    return 0;
}

static void *pending_pop(const uint8_t mac[6])
{
    int i;
    void *owner = NULL;
    for (i = 0; i < ESPD_NOW_PENDING_DEPTH; i++) {
        if (s_pending[i].owner && memcmp(s_pending[i].mac, mac, 6) == 0) {
            owner = s_pending[i].owner;
            s_pending[i].owner = NULL;
            return owner;
        }
    }
    return NULL;
}

static void espd_now_send_cb(const esp_now_send_info_t *tx_info,
    esp_now_send_status_t status)
{
    espd_now_event_t ev;
    uint8_t mac[6];

    if (!tx_info || !tx_info->des_addr)
        return;
    memcpy(mac, tx_info->des_addr, 6);

    memset(&ev, 0, sizeof(ev));
    ev.type = (status == ESP_NOW_SEND_SUCCESS)
        ? ESPD_NOW_EV_SEND_OK : ESPD_NOW_EV_SEND_FAIL;
    ev.owner = pending_pop(mac);
    memcpy(ev.mac, mac, 6);
    if (ev.type == ESPD_NOW_EV_SEND_FAIL)
        ev.reason = ESP_FAIL;

    if (s_ev_q && xQueueSend(s_ev_q, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full — dropping send result");
}

static void espd_now_recv_cb(const esp_now_recv_info_t *info,
    const uint8_t *data, int data_len)
{
    espd_now_event_t ev;

    if (!info || !data || data_len <= 0)
        return;
    if (data_len > ESPD_NOW_MAX_PAYLOAD)
        data_len = ESPD_NOW_MAX_PAYLOAD;

    memset(&ev, 0, sizeof(ev));
    ev.type = ESPD_NOW_EV_RECV;
    ev.owner = NULL;
    memcpy(ev.mac, info->src_addr, 6);
    ev.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    ev.len = data_len;
    memcpy(ev.payload, data, data_len);

    if (s_ev_q && xQueueSend(s_ev_q, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full — dropping recv frame");
}

void espd_now_set_event_handler(espd_now_event_cb cb)
{
    s_cb = cb;
}

bool espd_now_ready(void)
{
    return s_inited != 0;
}

void espd_now_init(void)
{
    esp_err_t err;

    if (s_inited)
        return;

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return;
    }
    err = esp_now_register_send_cb(espd_now_send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_send_cb: %s", esp_err_to_name(err));
        esp_now_deinit();
        return;
    }
    err = esp_now_register_recv_cb(espd_now_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_recv_cb: %s", esp_err_to_name(err));
        esp_now_deinit();
        return;
    }

    /* Optional global PMK from config.txt espnow_pmk= (32 hex chars = 16 bytes).
     * When set, encrypted peers use CCMP with this key. */
    if (g_espd_cfg.espnow_have_pmk) {
        err = esp_now_set_pmk((const uint8_t *)g_espd_cfg.espnow_pmk);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "esp_now_set_pmk: %s", esp_err_to_name(err));
        else
            ESP_LOGI(TAG, "PMK set (encrypted peers enabled)");
    }

    s_ev_q = xQueueCreate(ESPD_NOW_QUEUE_DEPTH, sizeof(espd_now_event_t));
    if (!s_ev_q) {
        ESP_LOGE(TAG, "event queue alloc failed");
        esp_now_deinit();
        return;
    }

    s_inited = 1;
    ESP_LOGI(TAG, "ready%s",
        g_espd_cfg.espnow_have_pmk ? " (encrypted)" : " (plaintext)");
}

esp_err_t espd_now_peer_add(const uint8_t mac[6], bool encrypt)
{
    esp_now_peer_info_t peer;

    if (!s_inited || !mac)
        return ESP_ERR_INVALID_STATE;
    if (encrypt && !g_espd_cfg.espnow_have_pmk)
        return ESP_ERR_INVALID_STATE; /* no PMK configured */

    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0; /* current channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypt;

    /* If already added, esp_now_add_peer returns ESP_ERR_ESPNOW_EXIST — treat
     * as success so the patch can call add idempotently. */
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
        return ESP_OK;
    return err;
}

esp_err_t espd_now_peer_del(const uint8_t mac[6])
{
    if (!s_inited || !mac)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_now_del_peer(mac);
    if (err == ESP_ERR_ESPNOW_NOT_FOUND)
        return ESP_OK;
    return err;
}

esp_err_t espd_now_send(void *owner, const uint8_t mac[6],
    const uint8_t *data, int len)
{
    esp_err_t err;

    if (!s_inited || !mac || !data || len <= 0 || len > ESPD_NOW_MAX_PAYLOAD)
        return ESP_ERR_INVALID_ARG;
    if (!pending_push(owner, mac)) {
        ESP_LOGW(TAG, "pending ring full — send rejected");
        return ESP_ERR_NO_MEM;
    }
    err = esp_now_send(mac, data, (size_t)len);
    if (err != ESP_OK) {
        pending_pop(mac); /* reclaim the slot since no cb will fire */
        return err;
    }
    return ESP_OK;
}

void espd_now_poll(void)
{
    espd_now_event_t ev;

    if (!s_ev_q || !s_cb)
        return;
    while (xQueueReceive(s_ev_q, &ev, 0) == pdTRUE)
        s_cb(&ev);
}

#else /* !ESPD_USE_ESPNOW */

void espd_now_init(void) {}
bool espd_now_ready(void) { return false; }
void espd_now_set_event_handler(espd_now_event_cb cb) { (void)cb; }
esp_err_t espd_now_peer_add(const uint8_t mac[6], bool encrypt)
{ (void)mac; (void)encrypt; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espd_now_peer_del(const uint8_t mac[6])
{ (void)mac; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espd_now_send(void *owner, const uint8_t mac[6],
    const uint8_t *data, int len)
{ (void)owner; (void)mac; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
void espd_now_poll(void) {}

#endif /* ESPD_USE_ESPNOW */
