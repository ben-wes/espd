/*
 * Waveshare ESP32-S3-AUDIO: I2S + ES8311/ES7210 hardware init for ESPD.
 */

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "espd_bsp_audio.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "espd_board_waveshare";

esp_err_t espd_bsp_audio_hw_init(const espd_bsp_audio_hw_params_t *params,
    espd_bsp_audio_hw_t *hw)
{
    i2s_std_config_t std_cfg;
    i2s_slot_mode_t slot_mode;
    uint32_t mclk_multiple;

    ESP_RETURN_ON_FALSE(params && hw, ESP_ERR_INVALID_ARG, TAG, "null arg");

    hw->spk = NULL;
    hw->mic = NULL;

    slot_mode = (params->channels >= 2) ? I2S_SLOT_MODE_STEREO
                                        : I2S_SLOT_MODE_MONO;

    std_cfg = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(params->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
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

    mclk_multiple = params->mclk_multiple ? params->mclk_multiple : BSP_AUDIO_MCLK_MULTIPLE;
    std_cfg.clk_cfg.mclk_multiple = mclk_multiple;

    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "bsp_audio_init");

    hw->spk = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(hw->spk, ESP_FAIL, TAG, "speaker codec init");

    hw->mic = bsp_audio_codec_microphone_init();
    if (!hw->mic)
        ESP_LOGW(TAG, "microphone codec init failed");

    return ESP_OK;
}
