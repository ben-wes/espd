/*
 * Custom composite USB descriptor (CDC [+ MSC] + MIDI).
 *
 * esp_tinyusb's auto descriptor builder does not support the MIDI class, so when
 * USB MIDI is enabled we hand-build the whole configuration descriptor and feed
 * it to the stack via tinyusb_config_t.descriptor. CDC dev-sync boards also use
 * a hand-built CDC-only descriptor (no MSC) so the host cannot probe a missing
 * mass-storage backend.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPD_USE_USB_OTG \
    && (CONFIG_ESPD_USE_USB_MIDI || CONFIG_ESPD_DEV_CDC_SYNC)

#include "tinyusb.h"

#if CONFIG_ESPD_USE_USB_MIDI
/* Fill cfg->descriptor with CDC [+MSC] + MIDI (device role picks CDC+MIDI only). */
void espd_usb_apply_midi_descriptor(tinyusb_config_t *cfg);
#endif

#if CONFIG_ESPD_DEV_CDC_SYNC
/* CDC serial only — no MSC (dev sync uses PUT, not a USB drive). */
void espd_usb_apply_cdc_sync_descriptor(tinyusb_config_t *cfg);
#endif

#endif /* CONFIG_ESPD_USE_USB_OTG && (MIDI || CDC_SYNC) */
