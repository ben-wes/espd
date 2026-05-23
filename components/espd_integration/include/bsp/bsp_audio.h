/*
 * Optional codec audio API ESPD uses for the BSP codec audio backend.
 */
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifndef BSP_AUDIO_SAMPLE_RATE_HZ
#define BSP_AUDIO_SAMPLE_RATE_HZ 48000
#endif

esp_err_t bsp_audio_init(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
