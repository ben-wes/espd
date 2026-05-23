#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t bsp_sdcard_mount(const char *mount_point);
bool bsp_sdcard_is_mounted(void);
