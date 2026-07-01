/*
 * Transport-agnostic host console sink (see espd_sync_io.h).
 */

#include "espd_sync_io.h"

#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>

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

void espd_sync_write(const void *data, size_t len)
{
    if (!data || len == 0)
        return;

#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if ESPD_DEV_SERIAL_SYNC_UART
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, len);
#elif ESPD_DEV_SERIAL_SYNC_USJ
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#else
    uart_write_bytes(UART_NUM_0, data, len);
#endif
#elif CONFIG_ESPD_DEV_CDC_SYNC
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
#endif
#if CONFIG_ESPD_WIFI_AP_SYNC
    if (espd_dev_wifi_client_active())
        espd_dev_wifi_write(data, len);
#endif
}

int espd_sync_log(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        size_t w = (size_t)n;
        if (w >= sizeof(buf))
            w = sizeof(buf) - 1;
        espd_sync_write(buf, w);
    }
    return n;
}
