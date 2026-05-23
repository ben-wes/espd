/*
 * Board codec hardware init hook. Weak default returns NOT_SUPPORTED;
 * espd_bsp_shim provides a strong override for managed esp-bsp boards.
 *
 * Opens I2S + codec devices only — esp_codec_dev_open() stays in main
 * (Pd sample rate, volume, gain).
 */
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"
#include <stdint.h>

typedef struct {
    int sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t mclk_multiple;
} espd_bsp_audio_hw_params_t;

typedef struct {
    esp_codec_dev_handle_t spk;
    esp_codec_dev_handle_t mic;
} espd_bsp_audio_hw_t;

esp_err_t espd_bsp_audio_hw_init(const espd_bsp_audio_hw_params_t *params,
    espd_bsp_audio_hw_t *hw);
