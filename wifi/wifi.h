#pragma once

#include <stdbool.h>
#include <freertos/FreeRTOS.h>

#ifdef ESPD_USE_WIFI

void espd_netif_ensure_init(void);
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
void wifi_ensure_hosted(void);
#endif
void wifi_prepare_phy(void);
void wifi_start_sta(void);
bool wifi_wait_sta(TickType_t ticks);

#if CONFIG_ESPD_WIFI_AP_SYNC
/** Apply AP SSID/password from config (or ESPD-XXXX default). */
void wifi_ap_configure(const char *ssid, const char *password);
/** Start SoftAP only (192.168.4.1). */
void wifi_start_ap(void);
/** SoftAP + STA when wifi_ssid= is set in config.txt. */
void wifi_start_apsta(void);
/** Tear down SoftAP; keep STA if it was running. */
void wifi_stop_ap(void);
bool wifi_ap_running(void);
/** Schedule AP shutdown after minutes (0 = no timer). */
void wifi_ap_boot_window_start(int minutes);
#endif

extern char wifi_ipaddr[];
extern char wifi_mac[];
extern char espd_wifi_ssid[];
extern char espd_wifi_password[];

#endif
