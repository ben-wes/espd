/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "../main/espd.h"
#include "esp_wifi_types_generic.h"
#ifdef ESPD_USE_WIFI
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
#include "esp_wifi_remote.h"
#include "esp_hosted.h"
#endif

#include "lwip/err.h"
#include "lwip/sys.h"

#define WIFI_MAXIMUM_RETRY  3

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/*
 * Boot contract (app_main in espd.c):
 *   1. wifi_prepare_phy()  — once, after config.txt credentials, before USB OTG
 *   2. (USB OTG init)
 *   3. wifi_start_sta()    — once, after USB; STA connect may overlap TinyUSB
 *   4. wifi_wait_sta()     — later, before Pd / legacy net (optional timeout)
 *
 * wifi_phy_init() is idempotent. wifi_start_sta() ensures PHY if called out of order.
 */

static const char *TAG = "ESPD";
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_phy_ready;
static bool s_wifi_sta_started;

char wifi_mac[80];
char wifi_ipaddr[20];

/* Common WiFi event handler logic */
typedef esp_err_t (*wifi_connect_func_t)(void);

static void wifi_event_handler_common(wifi_connect_func_t connect_func,
                                       esp_event_base_t event_base,
                                       int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        connect_func();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)event_data;
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            if (d->reason == WIFI_REASON_NO_AP_FOUND) {
                ESP_LOGW(TAG, "WIFI ssid=%s not found", espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            } else if (d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                ESP_LOGW(TAG, "WIFI wrong password for ssid=%s", espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }  else if (d->reason == WIFI_REASON_AUTH_FAIL) {
                ESP_LOGW(TAG, "WIFI authentication failed for ssid=%s", espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            } else {
                ESP_LOGW(TAG,
                    "WIFI failure: reason=%u rssi=%d",
                    (unsigned)d->reason, (int)d->rssi);
                connect_func();
                s_retry_num++;
                ESP_LOGI(TAG, "WIFI retry connection (%d/%d)", s_retry_num,
                    WIFI_MAXIMUM_RETRY);
            }
        } else {
            ESP_LOGI(TAG, "WIFI connection failed");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(wifi_ipaddr, sizeof(wifi_ipaddr),
            IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WIFI connected with IP %s", wifi_ipaddr);
        wifi_ipaddr[sizeof(wifi_ipaddr)-1] = 0;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
/* ESP-Hosted event handler (ESP32-P4)*/
static void hosted_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_handler_common(esp_wifi_remote_connect, event_base, event_id, event_data);
}
#else
/* Native WiFi event handler (ESP32/ESP32-S3) */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_handler_common(esp_wifi_connect, event_base, event_id, event_data);
}
#endif

void espd_netif_ensure_init(void)
{
    static int s_done = 0;
    if (s_done) return;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(e);
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(e);
    s_done = 1;
}

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
/* ESP-Hosted WiFi PHY init (ESP32-P4) */
static void wifi_phy_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_wifi_phy_ready)
        return;
    s_wifi_phy_ready = true;

    if (!s_wifi_event_group)
        s_wifi_event_group = xEventGroupCreate();

    espd_netif_ensure_init();
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &hosted_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, &hosted_event_handler, NULL, NULL));
}

static void wifi_sta_apply_config(void)
{
    wifi_config_t wifi_config = {0};

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
        espd_wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
        espd_wifi_password);
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_remote_set_ps(WIFI_PS_NONE);
}
#else
/* Native WiFi PHY init (ESP32/ESP32-S3) */
static void wifi_phy_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_wifi_phy_ready)
        return;
    s_wifi_phy_ready = true;

    if (!s_wifi_event_group)
        s_wifi_event_group = xEventGroupCreate();

    espd_netif_ensure_init();
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
}

static void wifi_sta_apply_config(void)
{
    wifi_config_t wifi_config = {0};

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
        espd_wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
        espd_wifi_password);
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_set_ps(WIFI_PS_NONE);
}
#endif

/* esp_wifi_init + event handlers. Call once before USB OTG (shared PHY ordering). */
void wifi_prepare_phy(void)
{
    if (s_wifi_phy_ready)
        return;
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    ESP_LOGI(TAG, "wifi_prepare_phy: esp_wifi_remote (ESP-Hosted)");
#else
    ESP_LOGI(TAG, "wifi_prepare_phy: native esp_wifi");
#endif
    wifi_phy_init();
}

/* Apply STA config and start. Does not block for DHCP — use wifi_wait_sta(). */
void wifi_start_sta(void)
{
    if (s_wifi_sta_started)
        return;

    wifi_prepare_phy();

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    wifi_sta_apply_config();
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    ESP_ERROR_CHECK(esp_wifi_remote_start());
#else
    ESP_ERROR_CHECK(esp_wifi_start());
#endif
    s_wifi_sta_started = true;

    {
        uint8_t sta_mac[6];
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
        ESP_ERROR_CHECK(esp_wifi_remote_get_mac(WIFI_IF_STA, sta_mac));
#else
        ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, sta_mac));
#endif
        /* ESP_LOGI(TAG, "wifi_start_sta: MAC %02x:%02x:%02x:%02x:%02x:%02x ssid=%s",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5],
            espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)"); */
        ESP_LOGI(TAG, "WIFI connecting AP ssid=%s",
            espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)");
        snprintf(wifi_mac, sizeof(wifi_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    }
}

bool wifi_wait_sta(TickType_t ticks)
{
    EventBits_t bits;

    if (!s_wifi_event_group)
        return false;

    bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & WIFI_CONNECTED_BIT)
        return true;
    if (bits & WIFI_FAIL_BIT)
        return false;

    bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", espd_wifi_ssid);
        printf("wifi: connected (ssid %s, ip %s)\n", espd_wifi_ssid, wifi_ipaddr);
        return true;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", espd_wifi_ssid);
        printf("wifi: connect failed (ssid %s)\n", espd_wifi_ssid);
        return false;
    }
    return false;
}
#endif  /* ESPD_USE_WIFI */
