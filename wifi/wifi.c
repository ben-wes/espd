/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "../main/espd.h"
#include "wifi.h"
#include "espd_bsp_sdcard.h"
#include "esp_wifi_types_generic.h"
#include "sdkconfig.h"

#if CONFIG_ESPD_WIFI_AP_SYNC
#include "espd_dev_http.h"
#endif

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

static const char *TAG = "ESPD";

/*
 * Boot contract (app_main in espd.c):
 *   0. wifi_ensure_hosted() — SDIO/C6 before uSD on ESP-Hosted boards
 *   1. wifi_prepare_phy()  — once, after config.txt credentials, before USB OTG
 *   2. (USB OTG init)
 *   3. wifi_start_sta()    — once, after USB; STA connect may overlap TinyUSB
 *   4. wifi_wait_sta()     — later, before Pd / legacy net (optional timeout)
 *
 * wifi_phy_init() is idempotent. wifi_start_sta() ensures PHY if called out of order.
 */

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
static bool s_hosted_ready;

/*
 * esp_hosted force-links an auto-init constructor (port_esp_hosted_host_init.c,
 * pulled in via the component's WHOLE_ARCHIVE property) that calls
 * esp_hosted_init() during do_global_ctors — before app_main, and before the C6
 * power/reset rail is sequenced. On the P4-NANO that early init hangs talking to
 * the C6 over SDIO. We can't drop the constructor by disabling WHOLE_ARCHIVE
 * (it also force-links esp_wifi_weak.c, the strong esp_wifi_* overrides), and no
 * Kconfig defers it. Instead we wrap the symbol at link time (-Wl,--wrap, see
 * main/CMakeLists.txt): the constructor-time call becomes a no-op and the real
 * init runs only once we flip the gate below from app_main. This keeps
 * esp_hosted a stock, unpatched managed component.
 */
extern esp_err_t __real_esp_hosted_init(void);
static volatile bool s_hosted_init_allowed;

esp_err_t __wrap_esp_hosted_init(void)
{
    if (!s_hosted_init_allowed)
        return ESP_OK; /* defer constructor-time init to wifi_ensure_hosted() */
    return __real_esp_hosted_init();
}

void wifi_ensure_hosted(void)
{
    if (s_hosted_ready)
        return;
    /* Open the gate so the wrapped esp_hosted_init() reaches the real impl.
     * Shared SDMMC host (slot 1 = C6) before uSD slot 0; BSP relies on this. */
    s_hosted_init_allowed = true;
#if CONFIG_IDF_TARGET_ESP32P4
    /* VDD_SDMMC (LDO ch 4) must be on before SDIO to the C6. BSP normally
     * acquires this during uSD mount; esp_hosted runs first on P4-NANO. */
    ESP_ERROR_CHECK(espd_bsp_sdmmc_pwr_on());
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
    ESP_LOGI(TAG, "esp_hosted init (C6 SDIO)...");
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    }
    s_hosted_ready = true;
    ESP_LOGI(TAG, "esp_hosted ready");
}
#endif

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_phy_ready;
static bool s_wifi_sta_started;
static bool s_wifi_driver_started;

char wifi_mac[80];
char wifi_ipaddr[20];

/* Common WiFi event handler logic */
typedef esp_err_t (*wifi_connect_func_t)(void);

static void wifi_event_handler_common(wifi_connect_func_t connect_func,
                                       esp_event_base_t event_base,
                                       int32_t event_id, void* event_data)
{
#if CONFIG_ESPD_WIFI_AP_SYNC
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        espd_dev_http_init();
    } else
#endif
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
#if CONFIG_ESPD_WIFI_AP_SYNC
    esp_netif_create_default_wifi_ap();
#endif
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
#if CONFIG_ESPD_WIFI_AP_SYNC
    esp_netif_create_default_wifi_ap();
#endif
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
    s_wifi_driver_started = true;
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

#if CONFIG_ESPD_WIFI_AP_SYNC

#include <esp_mac.h>

static bool s_wifi_ap_started;
static bool s_wifi_apsta;
static char s_ap_ssid[33];
static char s_ap_password[65];
static TimerHandle_t s_ap_boot_timer;

static void wifi_ap_apply_config(void)
{
    wifi_config_t ap = {0};
    size_t ssid_len = strlen(s_ap_ssid);

    if (ssid_len == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ESPD-%02X%02X", mac[4], mac[5]);
        ssid_len = strlen(s_ap_ssid);
    }
    if (ssid_len >= sizeof(ap.ap.ssid))
        ssid_len = sizeof(ap.ap.ssid) - 1;
    memcpy(ap.ap.ssid, s_ap_ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    if (strlen(s_ap_password) >= 8) {
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_ap_password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_AP, &ap));
#else
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#endif
}

void wifi_ap_configure(const char *ssid, const char *password)
{
    s_ap_ssid[0] = '\0';
    s_ap_password[0] = '\0';
    if (ssid && ssid[0])
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    if (password)
        snprintf(s_ap_password, sizeof(s_ap_password), "%s", password);
}

static void wifi_ap_start_common(bool with_sta)
{
    if (s_wifi_ap_started)
        return;

    wifi_prepare_phy();
    wifi_ap_apply_config();

    if (with_sta && espd_wifi_ssid[0]) {
        wifi_config_t sta = {0};
        snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", espd_wifi_ssid);
        snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", espd_wifi_password);
        sta.sta.pmf_cfg.capable = true;
        sta.sta.pmf_cfg.required = false;
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &sta));
#else
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
#endif
        s_wifi_apsta = true;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_num = 0;
    } else {
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_AP));
#else
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
#endif
        s_wifi_apsta = false;
    }

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    if (!s_wifi_driver_started)
        ESP_ERROR_CHECK(esp_wifi_remote_start());
#else
    if (!s_wifi_driver_started)
        ESP_ERROR_CHECK(esp_wifi_start());
#endif
    s_wifi_driver_started = true;
    if (s_wifi_apsta)
        s_wifi_sta_started = true;

    s_wifi_ap_started = true;
    ESP_LOGI(TAG, "WIFI SoftAP ssid=%s%s", s_ap_ssid[0] ? s_ap_ssid : "(auto)",
        with_sta && espd_wifi_ssid[0] ? " (APSTA)" : "");
    printf("wifi: SoftAP %s (https://192.168.4.1/ · TCP sync :4499)\n",
        s_ap_ssid[0] ? s_ap_ssid : "(auto)");
}

void wifi_start_ap(void)
{
    wifi_ap_start_common(false);
}

void wifi_start_apsta(void)
{
    wifi_ap_start_common(true);
}

void wifi_stop_ap(void)
{
    if (!s_wifi_ap_started)
        return;
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    if (s_wifi_apsta && espd_wifi_ssid[0])
        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    else
        ESP_ERROR_CHECK(esp_wifi_remote_stop());
#else
    if (s_wifi_apsta && espd_wifi_ssid[0])
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    else
        ESP_ERROR_CHECK(esp_wifi_stop());
#endif
    s_wifi_ap_started = false;
    s_wifi_apsta = false;
    ESP_LOGI(TAG, "WIFI SoftAP stopped");
    printf("wifi: SoftAP off\n");
}

bool wifi_ap_running(void)
{
    return s_wifi_ap_started;
}

static void wifi_ap_boot_timer_cb(TimerHandle_t t)
{
    (void)t;
    wifi_stop_ap();
}

void wifi_ap_boot_window_start(int minutes)
{
    if (minutes <= 0)
        return;
    if (!s_ap_boot_timer)
        s_ap_boot_timer = xTimerCreate("ap_win", pdMS_TO_TICKS((uint32_t)minutes * 60000U),
            pdFALSE, NULL, wifi_ap_boot_timer_cb);
    if (s_ap_boot_timer)
        xTimerStart(s_ap_boot_timer, 0);
    ESP_LOGI(TAG, "WIFI SoftAP boot window %d min", minutes);
}

#endif /* CONFIG_ESPD_WIFI_AP_SYNC */

#endif  /* ESPD_USE_WIFI */
