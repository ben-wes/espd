/*
 * BSP codec audio backend for Pd dac~ / adc~.
 *
 * Pd policy (rate, channels, volume, gain) lives here; board I/O via
 * espd_bsp_audio.h.
 */

#include "espd_audio.h"
#include "espd_bsp_audio.h"
#include "espd_config.h"
#include "espd_runtime_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "espd_audio";

#define ESPD_BSP_AUDIO_MCLK_MULTIPLE 256

struct espd_audio {
  void *spk;
#ifdef ESPD_USE_ADC
  void *mic;
#endif
  int sample_rate;
  int channels;
};

static espd_bsp_audio_codec_cfg_t espd_codec_cfg(int sample_rate,
                                                 int channels) {
  return (espd_bsp_audio_codec_cfg_t){
      .sample_rate_hz = sample_rate,
      .channels = (uint8_t)channels,
      .bits_per_sample = 16,
  };
}

esp_err_t espd_audio_init(espd_audio_t **out) {
  espd_audio_t *a;
  espd_bsp_audio_hw_t hw;
  espd_bsp_audio_hw_params_t hw_params;
  espd_bsp_audio_codec_cfg_t sample_cfg;

  if (!out)
    return ESP_ERR_INVALID_ARG;

  a = calloc(1, sizeof(*a));
  if (!a)
    return ESP_ERR_NO_MEM;

  a->sample_rate = espd_audio_sample_rate_hz();
  a->channels = IOCHANS;
  sample_cfg = espd_codec_cfg(a->sample_rate, a->channels);

  hw_params = (espd_bsp_audio_hw_params_t){
      .sample_rate_hz = a->sample_rate,
      .channels = (uint8_t)a->channels,
      .bits_per_sample = 16,
      .mclk_multiple = ESPD_BSP_AUDIO_MCLK_MULTIPLE,
  };

  ESP_RETURN_ON_ERROR(espd_bsp_audio_hw_init(&hw_params, &hw), TAG,
                      "espd_bsp_audio_hw_init");

  a->spk = hw.spk;
  ESP_RETURN_ON_ERROR(espd_bsp_audio_codec_open(a->spk, &sample_cfg), TAG,
                      "speaker open");
  (void)espd_bsp_audio_codec_set_out_mute(a->spk, true);

  {
    /* Mute, settle I2S with zeros, then unmute at target volume (no pop on
     * reset). */
    enum { ESPD_AUDIO_BLOCK_SAMPLES = 64, ESPD_AUDIO_PREROLL_BLOCKS = 16 };
    int16_t silence[ESPD_AUDIO_BLOCK_SAMPLES * IOCHANS];

    memset(silence, 0, sizeof(silence));
    for (int n = 0; n < ESPD_AUDIO_PREROLL_BLOCKS; n++)
      (void)espd_bsp_audio_codec_write(a->spk, silence, sizeof(silence));
  }

  ESP_RETURN_ON_ERROR(espd_bsp_audio_codec_set_out_mute(a->spk, false), TAG,
                      "speaker unmute");
  ESP_RETURN_ON_ERROR(espd_bsp_audio_codec_set_out_vol(a->spk, 100), TAG,
                      "speaker volume");

#ifdef ESPD_USE_ADC
  a->mic = hw.mic;
  if (a->mic) {
    if (espd_bsp_audio_codec_open(a->mic, &sample_cfg) != ESP_OK) {
      ESP_LOGW(TAG, "mic open failed");
      a->mic = NULL;
    } else if (espd_bsp_audio_codec_set_in_gain(a->mic, 30.0f) != ESP_OK) {
      ESP_LOGW(TAG, "mic gain failed");
    }
  }
#endif

  *out = a;
  ESP_LOGI(TAG, "ready: %d Hz, %d channel(s)", a->sample_rate, a->channels);
  return ESP_OK;
}

esp_err_t espd_audio_write(espd_audio_t *audio, const int16_t *pcm,
                           size_t samples) {
  if (!audio || !pcm || samples == 0)
    return ESP_ERR_INVALID_ARG;
  if (!audio->spk)
    return ESP_ERR_INVALID_STATE;

  return espd_bsp_audio_codec_write(audio->spk, pcm, samples * sizeof(int16_t));
}

esp_err_t espd_audio_read(espd_audio_t *audio, int16_t *pcm, size_t samples) {
  if (!audio || !pcm || samples == 0)
    return ESP_ERR_INVALID_ARG;

#ifndef ESPD_USE_ADC
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (!audio->mic)
    return ESP_ERR_NOT_SUPPORTED;

  return espd_bsp_audio_codec_read(audio->mic, pcm, samples * sizeof(int16_t));
#endif
}

int espd_audio_sample_rate(const espd_audio_t *audio) {
  return audio ? audio->sample_rate : espd_audio_sample_rate_hz();
}
