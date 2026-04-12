#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

esp_err_t espd_waveshare_s3_audio_init(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
