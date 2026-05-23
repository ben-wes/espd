/*
 * BSP codec audio backend for Pd dac~ / adc~.
 *
 * Pd policy (rate, channels, volume, gain) lives here; board I2S wiring and
 * codec chip setup via espd_bsp_audio_hw_init() in espd_bsp_shim.
 */

#include "espd_audio.h"
#include "espd_bsp_audio.h"
#include "espd_config.h"

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "espd_audio";

#define ESPD_BSP_AUDIO_RATE_HZ 48000
#define ESPD_BSP_AUDIO_MCLK_MULTIPLE 256

struct espd_audio {
    esp_codec_dev_handle_t spk;
#ifdef USEADC
    esp_codec_dev_handle_t mic;
#endif
    int sample_rate;
    int channels;
};

static esp_codec_dev_sample_info_t espd_codec_sample_info(int sample_rate)
{
    return (esp_codec_dev_sample_info_t){
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = sample_rate,
    };
}

esp_err_t espd_audio_init(espd_audio_t **out)
{
    espd_audio_t *a;
    espd_bsp_audio_hw_t hw;
    espd_bsp_audio_hw_params_t hw_params;
    esp_codec_dev_sample_info_t sample_cfg;

    if (!out)
        return ESP_ERR_INVALID_ARG;

    a = calloc(1, sizeof(*a));
    if (!a)
        return ESP_ERR_NO_MEM;

    a->sample_rate = ESPD_BSP_AUDIO_RATE_HZ;
    a->channels = IOCHANS;
    sample_cfg = espd_codec_sample_info(a->sample_rate);

    hw_params = (espd_bsp_audio_hw_params_t){
        .sample_rate_hz = a->sample_rate,
        .channels = (uint8_t)a->channels,
        .bits_per_sample = 16,
        .mclk_multiple = ESPD_BSP_AUDIO_MCLK_MULTIPLE,
    };

    ESP_RETURN_ON_ERROR(espd_bsp_audio_hw_init(&hw_params, &hw), TAG,
        "espd_bsp_audio_hw_init");

    a->spk = hw.spk;
    if (esp_codec_dev_open(a->spk, &sample_cfg) != ESP_CODEC_DEV_OK) {
        free(a);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(a->spk, 100) != ESP_CODEC_DEV_OK) {
        free(a);
        return ESP_FAIL;
    }

    {
        /* Flush DAC with digital zero before Pd may enable dsp~ output. */
        enum { ESPD_AUDIO_BLOCK_SAMPLES = 64, ESPD_AUDIO_PREROLL_BLOCKS = 4 };
        int16_t silence[ESPD_AUDIO_BLOCK_SAMPLES * IOCHANS];

        memset(silence, 0, sizeof(silence));
        for (int n = 0; n < ESPD_AUDIO_PREROLL_BLOCKS; n++)
            (void)esp_codec_dev_write(a->spk, silence, (int)sizeof(silence));
    }

#ifdef USEADC
    a->mic = hw.mic;
    if (a->mic) {
        if (esp_codec_dev_open(a->mic, &sample_cfg) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic open failed");
            a->mic = NULL;
        } else if (esp_codec_dev_set_in_gain(a->mic, 30.0f) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic gain failed");
        }
    }
#endif

    *out = a;
    ESP_LOGI(TAG, "ready: %d Hz, %d channel(s)", a->sample_rate, a->channels);
    return ESP_OK;
}

esp_err_t espd_audio_write(espd_audio_t *audio, const int16_t *pcm,
    size_t samples)
{
    if (!audio || !pcm || samples == 0)
        return ESP_ERR_INVALID_ARG;
    if (!audio->spk)
        return ESP_ERR_INVALID_STATE;

    size_t bytes = samples * sizeof(int16_t);
    int r = esp_codec_dev_write(audio->spk, (void *)pcm, (int)bytes);
    return (r == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t espd_audio_read(espd_audio_t *audio, int16_t *pcm, size_t samples)
{
    if (!audio || !pcm || samples == 0)
        return ESP_ERR_INVALID_ARG;

#ifndef USEADC
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!audio->mic)
        return ESP_ERR_NOT_SUPPORTED;

    size_t bytes = samples * sizeof(int16_t);
    int r = esp_codec_dev_read(audio->mic, pcm, (int)bytes);
    return (r == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
#endif
}

int espd_audio_sample_rate(const espd_audio_t *audio)
{
    return audio ? audio->sample_rate : ESPD_BSP_AUDIO_RATE_HZ;
}

int espd_audio_channels(const espd_audio_t *audio)
{
    return audio ? audio->channels : IOCHANS;
}
