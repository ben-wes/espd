/*
 * DAC audio backend using ESP32 internal DAC (GPIO25/26).
 * Converts int16 PCM to 8-bit DAC output for simple audio playback.
 */

#include "espd_audio.h"
#include "espd_config.h"
#include "espd_runtime_config.h"

#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

static const char *TAG = "espd_audio_dac";

struct espd_audio {
  dac_continuous_handle_t dac_handle;
  int sample_rate;
  int channels;
  dac_channel_mask_t chan_mask;
};

// Convert int16 sample to uint8 DAC value (0-255)
// int16 range: -32768 to 32767
// DAC range: 0 to 255
// Optimized for ESP32: use bit shift instead of multiply/divide
static inline uint8_t int16_to_dac(int16_t sample) {
  // Fast bit-shift conversion: (sample + 32768) >> 8
  // Equivalent to scaling by 255/65535 but much faster on ESP32
  // Input range [-32768, 32767] maps to [0, 255]
  // No clamping needed for valid int16 input
  return (uint8_t)((sample + 32768) >> 8);
}

esp_err_t espd_audio_init(espd_audio_t **out) {
  espd_audio_t *a;
  esp_err_t ret;

  if (!out)
    return ESP_ERR_INVALID_ARG;

  a = calloc(1, sizeof(*a));
  if (!a)
    return ESP_ERR_NO_MEM;

  a->sample_rate = espd_audio_sample_rate_hz();
  a->channels = IOCHANS;

  // Configure DAC channel mask based on board configuration
  // Default: use both channels for stereo (CH0=GPIO25, CH1=GPIO26)
#ifdef CONFIG_ESPD_DAC_USE_CH1_ONLY
  a->chan_mask = DAC_CHANNEL_MASK_CH1;
#elif defined(CONFIG_ESPD_DAC_USE_CH0_ONLY)
  a->chan_mask = DAC_CHANNEL_MASK_CH0;
#else
  // Default: use both channels for stereo
  a->chan_mask = DAC_CHANNEL_MASK_CH0 | DAC_CHANNEL_MASK_CH1;
#endif

  dac_continuous_config_t dac_cfg = {
      .chan_mask = a->chan_mask,
      .desc_num = (uint32_t)espd_audio_dma_desc_num(),
      .buf_size = (uint32_t)espd_audio_dma_frame_num() * 8, // 8-bit samples
      .freq_hz = a->sample_rate,
      /* Interleaved L/R from Pd needs ALTER; default SIMUL plays 2x samples/block. */
      .chan_mode = (a->chan_mask == (DAC_CHANNEL_MASK_CH0 | DAC_CHANNEL_MASK_CH1))
          ? DAC_CHANNEL_MODE_ALTER
          : DAC_CHANNEL_MODE_SIMUL,
  };

  ret = dac_continuous_new_channels(&dac_cfg, &a->dac_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create DAC continuous channel: %s",
             esp_err_to_name(ret));
    free(a);
    return ret;
  }

  ret = dac_continuous_enable(a->dac_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable DAC continuous: %s", esp_err_to_name(ret));
    dac_continuous_del_channels(a->dac_handle);
    free(a);
    return ret;
  }

  *out = a;
  const char *chan_str = (a->chan_mask == DAC_CHANNEL_MASK_CH1) ? "CH1 (GPIO26)"
                         : (a->chan_mask == DAC_CHANNEL_MASK_CH0)
                             ? "CH0 (GPIO25)"
                             : "CH0+CH1 (GPIO25+26)";
  ESP_LOGI(TAG, "ready: %d Hz, %d channel(s) on DAC %s", a->sample_rate,
           a->channels, chan_str);
  return ESP_OK;
}

esp_err_t espd_audio_write(espd_audio_t *audio, const int16_t *pcm,
                           size_t samples) {
  size_t bytes_written;
  esp_err_t ret;

  if (!audio || !pcm || samples == 0)
    return ESP_ERR_INVALID_ARG;

  // Determine if we have stereo DAC capability
  bool stereo_dac =
      (audio->chan_mask == (DAC_CHANNEL_MASK_CH0 | DAC_CHANNEL_MASK_CH1));

  // Convert int16 PCM to uint8 DAC samples
  uint8_t *dac_buf;
  size_t dac_samples;

  if (audio->channels == 1) {
    // Mono input: direct conversion
    dac_samples = samples;
    dac_buf = malloc(dac_samples);
    if (!dac_buf)
      return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < samples; i++) {
      dac_buf[i] = int16_to_dac(pcm[i]);
    }
  } else if (stereo_dac) {
    /* Interleaved L,R,... — ALTER mode maps even bytes→CH0, odd→CH1 (64 frames/block). */
    dac_samples = samples;
    dac_buf = malloc(dac_samples);
    if (!dac_buf)
      return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < samples; i++)
      dac_buf[i] = int16_to_dac(pcm[i]);
  } else {
    // Stereo input with mono DAC: mix to mono
    dac_samples = samples / 2;
    dac_buf = malloc(dac_samples);
    if (!dac_buf)
      return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < samples / 2; i++) {
      int32_t mixed = ((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) / 2;
      dac_buf[i] = int16_to_dac((int16_t)mixed);
    }
  }

  ret = dac_continuous_write(audio->dac_handle, dac_buf, dac_samples,
                             &bytes_written, portMAX_DELAY);

  free(dac_buf);
  return ret;
}

esp_err_t espd_audio_read(espd_audio_t *audio, int16_t *pcm, size_t samples) {
  // DAC is output-only, no capture support
  return ESP_ERR_NOT_SUPPORTED;
}

int espd_audio_sample_rate(const espd_audio_t *audio) {
  return audio ? audio->sample_rate : espd_audio_sample_rate_hz();
}
