/*
 * Waveshare board audio backend (bsp_waveshare_s3).
 */

#include "espd_audio.h"
#include "espd_config.h"

#include "bsp/waveshare_s3.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "espd_audio";

struct espd_audio {
    esp_codec_dev_handle_t spk;
#ifdef USEADC
    esp_codec_dev_handle_t mic;
#endif
    int sample_rate;
    int channels;
};

esp_err_t espd_audio_init(espd_audio_t **out)
{
    espd_audio_t *a;

    if (!out)
        return ESP_ERR_INVALID_ARG;

    a = calloc(1, sizeof(*a));
    if (!a)
        return ESP_ERR_NO_MEM;

    a->sample_rate = BSP_AUDIO_SAMPLE_RATE_HZ;
    a->channels = IOCHANS;

    ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "bsp_audio_init");

    a->spk = bsp_audio_codec_speaker_init();
    if (!a->spk) {
        free(a);
        return ESP_FAIL;
    }
#ifdef USEADC
    a->mic = bsp_audio_codec_microphone_init();
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
    return audio ? audio->sample_rate : BSP_AUDIO_SAMPLE_RATE_HZ;
}

int espd_audio_channels(const espd_audio_t *audio)
{
    return audio ? audio->channels : IOCHANS;
}
