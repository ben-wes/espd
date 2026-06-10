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

/* Legacy helpers */
bool espd_sdcard_main_pd_exists(void);
bool espd_storage_main_pd_exists(void);

void espd_storage_resolve_paths(void);
bool espd_storage_sdcard_ready(void);
bool espd_storage_flash_ready(void);

typedef struct {
    uint32_t total_kb;
    uint32_t free_kb;
    uint32_t used_kb;
} espd_storage_stats_t;

esp_err_t espd_storage_get_stats(const char *path, espd_storage_stats_t *stats);
