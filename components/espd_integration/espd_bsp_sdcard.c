/*
 * SD card mount glue (esp-bsp). Split out of espd_bsp_esp_bsp_io.c.
 *
 * Not a standalone TU: it is #included into the board-plugin io glue shim (see
 * gen_board_plugins.py) so it compiles with the selected BSP's headers, bsp_*
 * SD API and pin macros in scope. The generic (no-BSP) build instead links the
 * weak espd_bsp_sdcard_mount() stub in bsp_io_stubs.c.
 *
 * P4-NANO + ESP-Hosted: VDD_SDMMC (on-chip LDO ch 4) is shared by the uSD slot
 * and the C6 SDIO. wifi_ensure_hosted() calls espd_bsp_sdmmc_pwr_on() before
 * esp_hosted_init(); mount passes that handle in bsp_sdcard_cfg_t so the BSP
 * does not acquire the LDO a second time.
 */

#include "espd_bsp_sdcard.h"

#include <bsp/esp-bsp.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <string.h>

#if __has_include("sd_pwr_ctrl_by_on_chip_ldo.h")
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *SD_TAG = "espd_bsp_sd";

#if __has_include("sd_pwr_ctrl_by_on_chip_ldo.h")
static sd_pwr_ctrl_handle_t s_espd_sdmmc_pwr;
#endif

esp_err_t espd_bsp_sdmmc_pwr_on(void)
{
#if __has_include("sd_pwr_ctrl_by_on_chip_ldo.h")
    if (s_espd_sdmmc_pwr)
        return ESP_OK;
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_espd_sdmmc_pwr);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "SDMMC LDO enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

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
     * line on the expander before mounting; the BSP's mount path does not. */
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp) {
        esp_io_expander_set_dir(exp, BSP_SD_DET, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, BSP_SD_DET, 1);
    }
#endif

#if !BSP_CAPS_SDCARD
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
#else
#if defined(BSP_SDCARD_HAS_GET_HANDLE)
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;
#endif
    (void)mount_point;

#if __has_include("sd_pwr_ctrl_by_on_chip_ldo.h")
    if (s_espd_sdmmc_pwr) {
        bsp_sdcard_cfg_t cfg = {0};
        sdmmc_host_t sdhost = {0};
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &sdhost);
        sdhost.pwr_ctrl_handle = s_espd_sdmmc_pwr;
        cfg.host = &sdhost;
        return bsp_sdcard_sdmmc_mount(&cfg);
    }
#endif

    return bsp_sdcard_mount();
#endif
}
