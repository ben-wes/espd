/*
 * Optional SPIFFS partition (label from board_profile / espd.h) holding main.pd.
 */

#pragma once

#include <stdbool.h>

void espd_patch_store_init(void);
bool espd_patch_store_is_mounted(void);
bool espd_patch_store_main_pd_exists(void);
