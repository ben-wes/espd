/*
 * Optional I/O overrides for espd_board_waveshare_s3.
 *
 * Included by espd_integration/espd_bsp_esp_bsp_io.c when present in the
 * enabled board plugin's include path. Omit for boards where BSP button enum
 * order matches espd/din/0..N-1.
 */
#pragma once

#include "bsp/esp-bsp.h"

#define ESPD_BSP_BUTTON_COUNT 3
#define ESPD_BSP_BUTTON_MAP { \
    BSP_BUTTON_VOLUP, \
    BSP_BUTTON_PLAY, \
    BSP_BUTTON_VOLDOWN, \
}
