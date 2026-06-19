/*
 * USB OTG: TinyUSB, MSC (storage), CDC (serial), dev-sync.
 */
#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG \
    && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#define ESPD_DEV_SERIAL_SYNC_USJ 1
#elif !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG \
    && (CONFIG_ESP_CONSOLE_UART || CONFIG_ESP_CONSOLE_UART_CUSTOM)
#define ESPD_DEV_SERIAL_SYNC_UART 1
#endif
#endif

esp_err_t espd_usb_register_dynamic_storage(void);
esp_err_t espd_usb_mount_flash_early_vfs(void);

#if CONFIG_ESPD_USE_USB_OTG
bool espd_usb_start_after_wifi(void);
#endif

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
bool espd_usb_msc_storage_present(void);
bool espd_usb_msc_host_mounted(void);
bool espd_usb_wait_for_host(uint32_t timeout_ticks);
esp_err_t espd_usb_expose_msc_to_host(void);
esp_err_t espd_usb_ensure_msc_app_mount(void);
void espd_usb_drive_mode_wait(void);
esp_err_t espd_usb_msc_disable_and_remount_vfs(void);
void espd_usb_msc_disable_after_eject();
#endif

void espd_serial_sync_write(const void *data, size_t len);
int espd_serial_sync_log(const char *fmt, va_list args);
