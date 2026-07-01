/*
 * Transport-agnostic host console sink (see espd_sync_io.h).
 */

#include "espd_sync_io.h"

#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_ESPD_DEV_SERIAL_SYNC
#include <driver/uart.h>
#endif
#if ESPD_DEV_SERIAL_SYNC_USJ
#include <driver/usb_serial_jtag.h>
#endif
#if CONFIG_ESPD_DEV_CDC_SYNC
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#endif
#if CONFIG_ESPD_WIFI_AP_SYNC
#include "espd_dev_wifi.h"
#endif

#if ESPD_DEV_SERIAL_SYNC_USJ && CONFIG_ESPD_WIFI_AP_SYNC
static volatile bool s_usj_log_engaged;
#endif

void espd_sync_note_serial_rx(void)
{
#if ESPD_DEV_SERIAL_SYNC_USJ && CONFIG_ESPD_WIFI_AP_SYNC
    s_usj_log_engaged = true;
#else
    (void)0;
#endif
}

bool espd_sync_has_log_listener(void)
{
#if CONFIG_ESPD_WIFI_AP_SYNC
    if (espd_dev_wifi_client_active())
        return true;
#endif
#if ESPD_DEV_SERIAL_SYNC_USJ && CONFIG_ESPD_WIFI_AP_SYNC
    if (!usb_serial_jtag_is_connected()) {
        s_usj_log_engaged = false;
        return false;
    }
    return s_usj_log_engaged;
#elif ESPD_DEV_SERIAL_SYNC_USJ
    return usb_serial_jtag_is_connected();
#elif CONFIG_ESPD_DEV_SERIAL_SYNC
    return true;
#elif CONFIG_ESPD_DEV_CDC_SYNC
    return tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted();
#else
    return false;
#endif
}

static void espd_sync_write_serial(const void *data, size_t len, uint32_t usj_ticks,
        bool logs_only)
{
#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if ESPD_DEV_SERIAL_SYNC_USJ && CONFIG_ESPD_WIFI_AP_SYNC
    if (logs_only && !s_usj_log_engaged)
        return;
    if (!usb_serial_jtag_is_connected()) {
        s_usj_log_engaged = false;
        return;
    }
    {
        int n = usb_serial_jtag_write_bytes(data, len, usj_ticks);
        if (logs_only && n > 0)
            s_usj_log_engaged = true;
        (void)n;
    }
#elif ESPD_DEV_SERIAL_SYNC_UART
    (void)usj_ticks;
    (void)logs_only;
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, len);
#elif ESPD_DEV_SERIAL_SYNC_USJ
    (void)logs_only;
    if (!usb_serial_jtag_is_connected())
        return;
    (void)usb_serial_jtag_write_bytes(data, len, usj_ticks);
#else
    (void)usj_ticks;
    (void)logs_only;
    uart_write_bytes(UART_NUM_0, data, len);
#endif
#elif CONFIG_ESPD_DEV_CDC_SYNC
    (void)usj_ticks;
    (void)logs_only;
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted()) {
        size_t off = 0;
        while (off < len) {
            size_t w = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                    (const uint8_t *)data + off, len - off);
            if (w == 0)
                break;
            off += w;
            if (off < len)
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
#else
    (void)data;
    (void)len;
    (void)usj_ticks;
    (void)logs_only;
#endif
}

static void espd_sync_write_wifi(const void *data, size_t len, bool wait)
{
#if CONFIG_ESPD_WIFI_AP_SYNC
    if (espd_dev_wifi_client_active())
        (void)espd_dev_wifi_write(data, len, wait);
#else
    (void)data;
    (void)len;
    (void)wait;
#endif
}

void espd_sync_write(const void *data, size_t len)
{
    if (!data || len == 0)
        return;

#if ESPD_DEV_SERIAL_SYNC_USJ
    espd_sync_write_serial(data, len, pdMS_TO_TICKS(100), false);
#else
    espd_sync_write_serial(data, len, 0, false);
#endif
    espd_sync_write_wifi(data, len, true);
}

static volatile uint8_t s_sync_log_depth;

static void espd_sync_emit_log(const void *data, size_t len)
{
    espd_sync_write_serial(data, len, 0, true);
    if (s_sync_log_depth <= 1)
        espd_sync_write_wifi(data, len, false);
}

void espd_sync_print(const char *s)
{
    if (!s || !*s || !espd_sync_has_log_listener())
        return;
    espd_sync_emit_log(s, strlen(s));
}

int espd_sync_log(const char *fmt, va_list args)
{
    if (!espd_sync_has_log_listener())
        return 0;

    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        size_t w = (size_t)n;
        if (w >= sizeof(buf))
            w = sizeof(buf) - 1;
        s_sync_log_depth++;
        espd_sync_emit_log(buf, w);
        s_sync_log_depth--;
    }
    return n;
}
