/*
 * Shared esp_codec_dev glue for the BSP audio backend (all esp-bsp boards).
 * Compiled when CONFIG_ESPD_AUDIO_BACKEND_BSP_CODEC; **espd_board_*** must declare
 * **espressif/esp_codec_dev** in **idf_component.yml** (see Waveshare plugin).
 */

#include "espd_bsp_audio.h"

#include "esp_codec_dev.h"

static esp_codec_dev_sample_info_t espd_codec_dev_fs(
    const espd_bsp_audio_codec_cfg_t *cfg)
{
    return (esp_codec_dev_sample_info_t){
        .bits_per_sample = cfg->bits_per_sample,
        .channel = cfg->channels,
        .channel_mask = 0x03,
        .sample_rate = cfg->sample_rate_hz,
    };
}

esp_err_t espd_bsp_audio_codec_open(void *dev,
    const espd_bsp_audio_codec_cfg_t *cfg)
{
    esp_codec_dev_handle_t handle = (esp_codec_dev_handle_t)dev;
    esp_codec_dev_sample_info_t fs;

    if (!dev || !cfg)
        return ESP_ERR_INVALID_ARG;
    fs = espd_codec_dev_fs(cfg);
    if (esp_codec_dev_open(handle, &fs) != ESP_CODEC_DEV_OK)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t espd_bsp_audio_codec_write(void *dev, const void *data, size_t bytes)
{
    if (!dev || !data || bytes == 0)
        return ESP_ERR_INVALID_ARG;
    if (esp_codec_dev_write((esp_codec_dev_handle_t)dev, (void *)data, (int)bytes)
        != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t espd_bsp_audio_codec_read(void *dev, void *data, size_t bytes)
{
    if (!dev || !data || bytes == 0)
        return ESP_ERR_INVALID_ARG;
    if (esp_codec_dev_read((esp_codec_dev_handle_t)dev, data, (int)bytes)
        != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t espd_bsp_audio_codec_set_out_vol(void *dev, int vol_pct)
{
    if (!dev)
        return ESP_ERR_INVALID_ARG;
    if (esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)dev, vol_pct)
        != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t espd_bsp_audio_codec_set_in_gain(void *dev, float gain_db)
{
    if (!dev)
        return ESP_ERR_INVALID_ARG;
    if (esp_codec_dev_set_in_gain((esp_codec_dev_handle_t)dev, gain_db)
        != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
