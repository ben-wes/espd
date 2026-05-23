/*
 * BSP codec audio backend for Pd dac~ / adc~.
 *
 * Uses esp-bsp (Component Manager) when a managed board is selected; otherwise
 * the weak bsp_audio_* stubs from espd_integration apply.
 */

#include "espd_audio.h"
#include "espd_config.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include <stdlib.h>

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

static esp_codec_dev_sample_info_t espd_bsp_sample_info(void)
{
    return (esp_codec_dev_sample_info_t){
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = ESPD_BSP_AUDIO_RATE_HZ,
    };
}

static esp_err_t espd_bsp_audio_init_i2s(void)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ESPD_BSP_AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    std_cfg.clk_cfg.mclk_multiple = ESPD_BSP_AUDIO_MCLK_MULTIPLE;
    return bsp_audio_init(&std_cfg);
}

esp_err_t espd_audio_init(espd_audio_t **out)
{
    espd_audio_t *a;
    esp_codec_dev_sample_info_t sample_cfg;

    if (!out)
        return ESP_ERR_INVALID_ARG;

    a = calloc(1, sizeof(*a));
    if (!a)
        return ESP_ERR_NO_MEM;

    a->sample_rate = ESPD_BSP_AUDIO_RATE_HZ;
    a->channels = IOCHANS;
    sample_cfg = espd_bsp_sample_info();

    ESP_RETURN_ON_ERROR(espd_bsp_audio_init_i2s(), TAG, "bsp_audio_init");

    a->spk = bsp_audio_codec_speaker_init();
    if (!a->spk) {
        free(a);
        return ESP_FAIL;
    }
    if (esp_codec_dev_open(a->spk, &sample_cfg) != ESP_CODEC_DEV_OK) {
        free(a);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(a->spk, 100) != ESP_CODEC_DEV_OK) {
        free(a);
        return ESP_FAIL;
    }

#ifdef USEADC
    a->mic = bsp_audio_codec_microphone_init();
    if (a->mic) {
        esp_err_t mic_err = esp_codec_dev_open(a->mic, &sample_cfg);
        if (mic_err != ESP_CODEC_DEV_OK) {
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
