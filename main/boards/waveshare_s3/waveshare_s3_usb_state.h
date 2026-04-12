/*
 * Optional USB VBUS sense for mode switching (MSC vs audio) on Waveshare S3.
 * Set ESPD_WAVESHARE_USB_VBUS_GPIO in board_profile.h after schematic review.
 */

#pragma once

#include <stdbool.h>

void espd_waveshare_usb_vbus_init(void);
bool espd_waveshare_usb_vbus_gpio_configured(void);
bool espd_waveshare_usb_vbus_present(void);

/* If VBUS is present at boot: block here until unplugged (MSC placeholder). */
void espd_waveshare_s3_run_disc_mode_until_unplug(void);

/* Call from the main loop: debounced VBUS change triggers esp_restart() for
 * mode switch (works with battery: unplugging USB does not brown out the SoC).
 */
void espd_waveshare_s3_poll_usb_hotplug_restart(void);
