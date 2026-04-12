/*
 * Optional USB VBUS sense for mode switching (MSC vs audio) on Waveshare S3.
 *
 * Sources (pick one in board_profile.h):
 *   - GPIO on ESP32-S3 (divider from Type-C VBUS)
 *   - Waveshare UPS HAT (E) @ I2C 0x2D, charging reg 0x02 bit 5 == VBUS powered
 */

#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

void espd_waveshare_usb_vbus_init(void);
bool espd_waveshare_usb_vbus_monitoring_configured(void);
bool espd_waveshare_usb_vbus_present(void);

/* GPIO/UPS VBUS stub, then optional TinyUSB host-enumeration wait (see board_profile.h). */
void espd_waveshare_s3_usb_boot_before_pd(void);

/* After ES8311 has created the shared I2C master bus (same SCL/SDA as UPS). */
void espd_waveshare_usb_state_register_i2c_bus(i2c_master_bus_handle_t bus);

/* If VBUS is present at boot: block here until unplugged (MSC placeholder). */
void espd_waveshare_s3_run_disc_mode_until_unplug(void);

/* Call from the main loop: debounced VBUS change triggers esp_restart() for
 * mode switch (works with battery: unplugging USB does not brown out the SoC).
 */
void espd_waveshare_s3_poll_usb_hotplug_restart(void);
