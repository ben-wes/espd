/*
 * Local patch/config storage priority (by mounted medium, not file presence):
 *   SD card (if built and mounted) → internal flash /storage.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Resolve active store (SD mount may not exist yet). */
void espd_storage_init(void);

/** Mount SD card (if enabled) and re-resolve active store. Idempotent. */
esp_err_t espd_storage_mount_sdcard(void);

/** config.txt on the active store only, or NULL if none there. */
const char *espd_storage_config_path(void);

/** Active patch directory (/sdcard or /storage), or NULL. */
const char *espd_storage_main_pd_mount_dir(void);

/* Legacy helpers (prefer espd_storage_* above). */
bool espd_patch_store_is_mounted(void);
bool espd_patch_store_main_pd_exists(void);
bool espd_sdcard_main_pd_exists(void);
bool espd_storage_main_pd_exists(void);

/** Re-resolve after mount changes or CDC PUT. */
void espd_storage_refresh_paths(void);

bool espd_storage_sdcard_ready(void);
bool espd_storage_flash_ready(void);
