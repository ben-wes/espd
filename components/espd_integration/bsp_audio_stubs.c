/*
 * Weak defaults for optional bsp_audio.h (no codec BSP linked).
 */

#include "bsp/bsp_audio.h"

__attribute__((weak)) esp_err_t bsp_audio_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return NULL;
}

__attribute__((weak)) esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    return NULL;
}
