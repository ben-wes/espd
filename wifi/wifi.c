/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "../main/espd.h"
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

#include "lwip/err.h"
#include "lwip/sys.h"

#define EXAMPLE_ESP_MAXIMUM_RETRY  50

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "ESPD";
static int s_retry_num = 0;
char wifi_mac[80];
char wifi_ipaddr[20];

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG,
            "STA disconnected: reason=%u rssi=%d (e.g. 201=no AP, 2=auth, 15=wrong pwd)",
            (unsigned)d->reason, (int)d->rssi);
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num,
                EXAMPLE_ESP_MAXIMUM_RETRY);
        } else {
            ESP_LOGI(TAG,"connect to the AP fail");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        /* Do not log the address on default log level; wifi_ipaddr kept for net_hello etc. */
        ESP_LOGI(TAG, "STA DHCP complete");
        snprintf(wifi_ipaddr, sizeof(wifi_ipaddr),
            IPSTR, IP2STR(&event->ip_info.ip));
        wifi_ipaddr[sizeof(wifi_ipaddr)-1] = 0;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Idempotent bring-up of the lwIP TCP/IP thread and default event loop.
 * Safe to call multiple times (returns OK if already initialized). Exposed
 * so that app_main can ensure the TCP/IP stack exists even when Wi-Fi is
 * intentionally skipped, otherwise Pd's [netreceive]/[netsend] call socket()
 * before the tcpip thread is up and abort() inside lwIP. */
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

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    espd_netif_ensure_init();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", espd_wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             espd_wifi_password);
    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    /* .threshold.authmode = WIFI_AUTH_WPA2_PSK, */
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config) );

    uint8_t sta_mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, sta_mac));
    ESP_LOGI(TAG, "wifi_init_sta finished, STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    snprintf(wifi_mac, sizeof(wifi_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    
    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", espd_wifi_ssid);
        /* sdkconfig can set CONFIG_LOG_DEFAULT_LEVEL_WARN; ESP_LOGI is hidden then. */
        printf("wifi: connected (ssid %s, ip %s)\n", espd_wifi_ssid, wifi_ipaddr);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s (check password, 2.4 GHz, reason in log)",
                 espd_wifi_ssid);
        printf("wifi: connect failed (ssid %s) — see esp_wifi disconnect reasons above\n",
               espd_wifi_ssid);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void wifi_init(void)
{
    /* NVS is initialized once in app_main (espd.c) before SPIFFS / Pd boot. */
#if CONFIG_ESP_HOST_WIFI_ENABLED
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (ESP-Hosted / C6 coprocessor)");
#else
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
#endif
    wifi_init_sta();
}
#endif  /* ESPD_USE_WIFI */
