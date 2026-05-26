/*
 * Local patch/config storage: SD card first, internal SPIFFS second, USB MSC third.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Mount SPIFFS patch store and resolve config.txt / main.pd (SD not mounted yet). */
void espd_storage_init(void);

/** Mount SD card (if enabled) and re-probe config.txt / main.pd paths. Idempotent. */
esp_err_t espd_storage_mount_sdcard(void);

/** Path to the first existing config.txt, or NULL (SD → SPIFFS → USB MSC). */
const char *espd_storage_config_path(void);

/** Mount dir containing main.pd (e.g. /sdcard), or NULL if not found. */
const char *espd_storage_main_pd_mount_dir(void);

bool espd_storage_spiffs_mounted(void);

/* Legacy helpers (prefer espd_storage_* above). */
bool espd_patch_store_is_mounted(void);
bool espd_patch_store_main_pd_exists(void);
bool espd_sdcard_main_pd_exists(void);
bool espd_storage_main_pd_exists(void);

/** Re-probe config.txt / main.pd after files change on SD (e.g. CDC PUT). */
void espd_storage_refresh_paths(void);

/** True when the SD card VFS mount is present. */
bool espd_storage_sdcard_ready(void);
