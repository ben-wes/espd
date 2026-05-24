/*
 * Board-neutral audio backend for Pd dac~ / adc~ (PCM at DEFDACBLKSIZE).
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct espd_audio espd_audio_t;

/** Initialize playback (and capture when ESPD_USE_ADC is enabled). */
esp_err_t espd_audio_init(espd_audio_t **out);

/** Write interleaved int16 PCM (frames * channels samples). */
esp_err_t espd_audio_write(espd_audio_t *audio, const int16_t *pcm,
    size_t samples);

/** Read interleaved int16 PCM. Returns ESP_ERR_NOT_SUPPORTED when capture off. */
esp_err_t espd_audio_read(espd_audio_t *audio, int16_t *pcm, size_t samples);

int espd_audio_sample_rate(const espd_audio_t *audio);
int espd_audio_channels(const espd_audio_t *audio);
