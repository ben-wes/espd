/*
 * Waveshare ESP32-S3-AUDIO-Board TF slot (wiki pin table):
 *   CLK GPIO40, CMD GPIO42, D0 GPIO41; SD_D3/CS on TCA9555 EXIO3.
 */

#include "boards/waveshare_s3/waveshare_s3_sdcard.h"

#include "bsp/waveshare_s3.h"
#include "espd.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "waveshare_sdcard";
static const char *SD_HOST_TAG = "SD_HOST";

static bool s_sd_mounted;

#ifndef ESPD_WAVESHARE_SD_PIN_CLK
#define ESPD_WAVESHARE_SD_PIN_CLK 40
#endif
#ifndef ESPD_WAVESHARE_SD_PIN_CMD
#define ESPD_WAVESHARE_SD_PIN_CMD 42
#endif
#ifndef ESPD_WAVESHARE_SD_PIN_D0
#define ESPD_WAVESHARE_SD_PIN_D0 41
#endif

bool espd_waveshare_s3_sdcard_is_mounted(void)
{
    return s_sd_mounted;
}

esp_err_t espd_waveshare_s3_sdcard_mount(void)
{
    if (s_sd_mounted)
        return ESP_OK;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus) {
        esp_err_t e = bsp_waveshare_io_expander_sd_cs_high();
        if (e != ESP_OK)
            ESP_LOGW(TAG, "EXIO3 SD CS: %s (continuing)", esp_err_to_name(e));
    } else {
        esp_err_t e = bsp_waveshare_io_expander_sd_cs_high();
        if (e != ESP_OK)
            ESP_LOGW(TAG, "EXIO3 SD CS (ephemeral I2C): %s (continuing)", esp_err_to_name(e));
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = (gpio_num_t)ESPD_WAVESHARE_SD_PIN_CLK;
    slot_config.cmd = (gpio_num_t)ESPD_WAVESHARE_SD_PIN_CMD;
    slot_config.d0 = (gpio_num_t)ESPD_WAVESHARE_SD_PIN_D0;
    slot_config.width = 1;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /*
     * ESP32-S3 SD host doesn't implement input delayline tuning, and IDF logs a
     * one-time fallback warning from SD_HOST during mount. Keep warnings enabled
     * globally, but silence this known benign fallback at the source tag.
     */
    esp_err_t ret = ESP_FAIL;
    sdmmc_card_t *card = NULL;
    const int speed_candidates_khz[] = {40000, 25000, 20000, 10000};
    size_t i;

    esp_log_level_set(SD_HOST_TAG, ESP_LOG_ERROR);
    for (i = 0; i < sizeof(speed_candidates_khz) / sizeof(speed_candidates_khz[0]); i++) {
        host.max_freq_khz = speed_candidates_khz[i];
        ret = esp_vfs_fat_sdmmc_mount(ESPD_SDCARD_MOUNT, &host, &slot_config,
                                      &mount_config, &card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at %s (%d kHz, 1-bit)", ESPD_SDCARD_MOUNT,
                     host.max_freq_khz);
            printf("sdcard: mounted %s @ %d kHz (1-bit)\n", ESPD_SDCARD_MOUNT,
                   host.max_freq_khz);
            break;
        }
        ESP_LOGW(TAG, "SD mount failed at %d kHz: %s", host.max_freq_khz, esp_err_to_name(ret));
    }
    esp_log_level_set(SD_HOST_TAG, ESP_LOG_WARN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card at %s: %s", ESPD_SDCARD_MOUNT,
                 esp_err_to_name(ret));
        return ret;
    }

    (void)card;
    s_sd_mounted = true;
    return ESP_OK;
}
