/*
 * Host console I/O for dev sync: protocol replies, Pd print, ESP_LOG.
 * Backends: USB Serial JTAG / UART, OTG CDC, WiFi SoftAP TCP (when connected).
 */
#pragma once

#include <stddef.h>
#include <stdarg.h>
#include "sdkconfig.h"

#if CONFIG_ESPD_DEV_SERIAL_SYNC
/* Host tools (Web Serial, esptool, espd_sync.py) use the USB Serial JTAG
 * interface when the chip has one — independent of which port is IDF console. */
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#define ESPD_DEV_SERIAL_SYNC_USJ 1
#elif CONFIG_ESP_CONSOLE_UART || CONFIG_ESP_CONSOLE_UART_CUSTOM
#define ESPD_DEV_SERIAL_SYNC_UART 1
#endif
#endif

void espd_sync_write(const void *data, size_t len);
int espd_sync_log(const char *fmt, va_list args);
