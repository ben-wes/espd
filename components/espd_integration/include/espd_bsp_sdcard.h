/*
 * Board SD mount hook. Weak default forwards to bsp_sdcard_mount(); managed
 * boards (espd_board_*) provide a strong override when esp-bsp APIs differ.
 */
#pragma once

#include "esp_err.h"

/* VDD_SDMMC on-chip LDO (ch 4 on P4-NANO). Idempotent; call from wifi_ensure_hosted()
 * before esp_hosted_init when SDIO shares the rail with uSD. */
esp_err_t espd_bsp_sdmmc_pwr_on(void);

esp_err_t espd_bsp_sdcard_mount(const char *mount_point);
