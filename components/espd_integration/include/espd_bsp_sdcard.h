/*
 * Board SD mount hook. Weak default forwards to bsp_sdcard_mount(); managed
 * boards (espd_bsp_shim) provide a strong override when esp-bsp APIs differ.
 */
#pragma once

#include "esp_err.h"

esp_err_t espd_bsp_sdcard_mount(const char *mount_point);
