/*
 * Generic I2S audio backend (no external codec driver).
 */

#include "espd_audio.h"
#include "espd_config.h"
#include "espd_runtime_config.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

static const char *TAG = "espd_audio";

struct espd_audio {
    i2s_chan_handle_t tx;
#ifdef USEADC
    i2s_chan_handle_t rx;
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

    a->sample_rate = espd_audio_sample_rate_hz();
    a->channels = IOCHANS;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
        I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = (uint32_t)espd_audio_dma_desc_num();
    chan_cfg.dma_frame_num = (uint32_t)espd_audio_dma_frame_num();
#ifdef USEADC
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &a->tx, &a->rx), TAG,
        "i2s_new_channel");
#else
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &a->tx, NULL), TAG,
        "i2s_new_channel");
#endif

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(a->sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            IOCHANS > 1 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BIT_CLOCK,
            .ws = PIN_WORD_SELECT,
            .dout = PIN_DATA_OUT,
            .din = PIN_DATA_IN,
            .invert_flags = {false, false, false},
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(a->tx, &std_cfg), TAG,
        "i2s tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(a->tx), TAG, "i2s tx enable");
#ifdef USEADC
    if (a->rx) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(a->rx, &std_cfg), TAG,
            "i2s rx init");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(a->rx), TAG, "i2s rx enable");
    }
#endif

    *out = a;
    ESP_LOGI(TAG, "ready: %d Hz, %d channel(s)", a->sample_rate, a->channels);
    return ESP_OK;
}

esp_err_t espd_audio_write(espd_audio_t *audio, const int16_t *pcm,
    size_t samples)
{
    size_t transferred = 0;
    size_t bytes;

    if (!audio || !pcm || samples == 0)
        return ESP_ERR_INVALID_ARG;

    bytes = samples * sizeof(int16_t);
    return i2s_channel_write(audio->tx, pcm, bytes, &transferred,
        portMAX_DELAY);
}

esp_err_t espd_audio_read(espd_audio_t *audio, int16_t *pcm, size_t samples)
{
    size_t transferred = 0;
    size_t bytes;

    if (!audio || !pcm || samples == 0)
        return ESP_ERR_INVALID_ARG;

#ifndef USEADC
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!audio->rx)
        return ESP_ERR_NOT_SUPPORTED;

    bytes = samples * sizeof(int16_t);
    return i2s_channel_read(audio->rx, pcm, bytes, &transferred,
        portMAX_DELAY);
#endif
}

int espd_audio_sample_rate(const espd_audio_t *audio)
{
    return audio ? audio->sample_rate : espd_audio_sample_rate_hz();
}

int espd_audio_channels(const espd_audio_t *audio)
{
    return audio ? audio->channels : IOCHANS;
}
