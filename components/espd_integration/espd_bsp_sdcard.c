/*
 * SD card mount glue (esp-bsp). Split out of espd_bsp_esp_bsp_io.c.
 *
 * Not a standalone TU: it is #included into the board-plugin io glue shim (see
 * gen_board_plugins.py) so it compiles with the selected BSP's headers, bsp_*
 * SD API and pin macros in scope. The generic (no-BSP) build instead links the
 * weak espd_bsp_sdcard_mount() stub in bsp_io_stubs.c.
 */


#include "espd_bsp_sdcard.h"

#include <bsp/esp-bsp.h>
#include <esp_log.h>
#include <string.h>

static const char *SD_TAG = "espd_bsp_sd";

esp_err_t espd_bsp_sdcard_mount(const char *mount_point)
{
#ifdef BSP_SD_MOUNT_POINT
    if (mount_point && strcmp(mount_point, BSP_SD_MOUNT_POINT) != 0)
        ESP_LOGW(SD_TAG, "mount_point %s ignored (BSP uses %s)",
            mount_point, BSP_SD_MOUNT_POINT);
#endif

#ifdef BSP_SD_DET
    /* BSP_SD_DET is an IO-EXPANDER pin (encoded as GPIO_NUM_MAX + n, e.g. the
     * TCA9555 EXIO3 on Waveshare), NOT a GPIO — the BSP uses this same encoding
     * for BSP_LCD_EN / BSP_TOUCH_EN with esp_io_expander_set_*(). Drive the SD
     * line on the expander before mounting; the BSP's mount path does not.
     * (No `!= GPIO_NUM_NC` guard: an encoded expander pin is always positive and
     * can never equal the GPIO "not-connected" sentinel -1.) */
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp) {
        esp_io_expander_set_dir(exp, BSP_SD_DET, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, BSP_SD_DET, 1);
    }
#endif

#if __has_include("bsp/esp_bsp_sdcard.h")
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;
    bsp_sdcard_cfg_t cfg = {0};
    return bsp_sdcard_sdmmc_mount(&cfg);
#elif BSP_CAPS_SDCARD && defined(BSP_SDCARD_HAS_GET_HANDLE)
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;
    (void)mount_point;
    bsp_sdcard_cfg_t cfg = {0};
    return bsp_sdcard_sdmmc_mount(&cfg);
#elif BSP_CAPS_SDCARD
    (void)mount_point;
    bsp_sdcard_cfg_t cfg = {0};
    return bsp_sdcard_sdmmc_mount(&cfg);
#else
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
