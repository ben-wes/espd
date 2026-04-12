#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

esp_err_t espd_waveshare_s3_audio_init(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);

/** Interleaved PCM (e.g. int16 stereo) to ES8311 via esp_codec_dev (volume + I2S lock). */
int espd_waveshare_s3_codec_write(void *data, int len_bytes);

#ifdef USEADC
/** Interleaved PCM from ES7210 (onboard mics) via esp_codec_dev; returns 0 on success. */
int espd_waveshare_s3_codec_read(void *data, int len_bytes);
#endif
