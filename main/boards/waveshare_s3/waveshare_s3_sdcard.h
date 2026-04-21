#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Mount TF card at ESPD_SDCARD_MOUNT (default /sdcard) via SDMMC 1-line + FAT. Idempotent. */
esp_err_t espd_waveshare_s3_sdcard_mount(void);

bool espd_waveshare_s3_sdcard_is_mounted(void);
