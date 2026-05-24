/*
 * Board codec hooks for the BSP audio backend.
 *
 * espd_board_* plugins implement espd_bsp_audio_hw_init() (kit-specific).
 * espd_integration/espd_bsp_codec_dev.c implements codec I/O when the BSP
 * backend is selected; espd_integration declares espressif/esp_codec_dev
 * itself, so plugins inherit it transitively via their `path:` dep on
 * espd_integration in idf_component.yml.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t mclk_multiple;
} espd_bsp_audio_hw_params_t;

typedef struct {
    void *spk;
    void *mic;
} espd_bsp_audio_hw_t;

typedef struct {
    int sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} espd_bsp_audio_codec_cfg_t;

esp_err_t espd_bsp_audio_hw_init(const espd_bsp_audio_hw_params_t *params,
    espd_bsp_audio_hw_t *hw);

esp_err_t espd_bsp_audio_codec_open(void *dev,
    const espd_bsp_audio_codec_cfg_t *cfg);
esp_err_t espd_bsp_audio_codec_write(void *dev, const void *data, size_t bytes);
esp_err_t espd_bsp_audio_codec_read(void *dev, void *data, size_t bytes);
esp_err_t espd_bsp_audio_codec_set_out_vol(void *dev, int vol_pct);
esp_err_t espd_bsp_audio_codec_set_in_gain(void *dev, float gain_db);
