/*
 * CDC dev protocol: PUT files to active local target (/sdcard or /storage), RELOAD main.pd.
 */

#pragma once
#include <stddef.h>
#include <sdkconfig.h>
#if CONFIG_ESPD_DEV_SYNC
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#if CONFIG_ESPD_DEV_CDC_SYNC
#include "tinyusb_cdc_acm.h"
/** TinyUSB CDC RX callback (set in tinyusb_cdcacm_init). */
void espd_dev_cdc_rx_cb(int itf, cdcacm_event_t *event);
#endif

void espd_dev_init(void);
bool espd_dev_reload_pending(void);
void espd_dev_clear_reload_pending(void);
const char *espd_dev_reload_dir(void);
/** Copy one queued MSG line (audio thread only); clears pending. */
bool espd_dev_pdmsg_take(char *out, size_t outsz);

/** Audio-thread hook: pending RELOAD/MSG. Returns true while a host dev-sync batch is
 *  in progress (PUT/LIST/… until RELOAD/STATUS); skip Pd ticks when true. */
bool espd_dev_sync_poll(void);

#if CONFIG_ESPD_DEV_SYNC
/** espd_dev FreeRTOS task (WiFi transport notifier). */
TaskHandle_t espd_dev_task_handle(void);
#endif

extern bool g_espd_pd_running;
