/*
 * CDC dev protocol: PUT files to /sdcard, RELOAD main.pd (rapid dev, SD required).
 */

#pragma once

#include <stdbool.h>
#include "sdkconfig.h"

#if CONFIG_ESPD_DEV_CDC_SYNC
#include "tinyusb_cdc_acm.h"
/** TinyUSB CDC RX callback (set in tinyusb_cdcacm_init). */
void espd_dev_cdc_rx_cb(int itf, cdcacm_event_t *event);
#endif

void espd_dev_init(void);
bool espd_dev_reload_pending(void);
void espd_dev_clear_reload_pending(void);
