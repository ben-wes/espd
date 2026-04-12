/*
 * I2S + I2C bring-up for Waveshare ESP32-S3-AUDIO-Board (ES8311).
 * Uses esp_codec_dev (ESP-IDF component manager), not full ADF.
 */

#include "boards/waveshare_s3/board_profile.h"
#include "boards/waveshare_s3/waveshare_s3_exio.h"
#include "boards/waveshare_s3/waveshare_s3_usb_state.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_log.h"
#include "es8311_codec.h"

static const char *TAG = "waveshare_s3";

#define ESPD_SAMPLE_RATE_HZ 48000
#define MCLK_MULTIPLE 256

static i2c_master_bus_handle_t s_i2c_bus;
static esp_codec_dev_handle_t s_codec;

static esp_err_t es8311_codec_init(i2s_chan_handle_t tx_h, i2s_chan_handle_t rx_h)
{
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_if_t *es8311_if;
    const audio_codec_gpio_if_t *gpio_if;
    es8311_codec_cfg_t es8311_cfg;
    esp_codec_dev_cfg_t dev_cfg = {0};
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = ESPD_SAMPLE_RATE_HZ,
    };

    i2c_master_bus_config_t i2c_mst_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = ESPD_WAVESHARE_I2C_SDA_GPIO,
        .scl_io_num = ESPD_WAVESHARE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_mst_cfg, &s_i2c_bus), TAG,
        "i2c_new_master_bus");

    if (espd_waveshare_exio_apply_usb_mux(s_i2c_bus) != ESP_OK)
        ESP_LOGW(TAG, "TCA9554 EXIO mux failed (USB may not enumerate on Type-C)");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio_codec_new_i2c_ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_h,
        .tx_handle = tx_h,
    };
    data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio_codec_new_i2s_data");

    gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio_codec_new_gpio");

    es8311_cfg = (es8311_codec_cfg_t){
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = -1,
        .pa_reverted = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
        .mclk_div = MCLK_MULTIPLE,
    };
    es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "es8311_codec_new");

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = es8311_if;
    dev_cfg.data_if = data_if;
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "esp_codec_dev_new");

    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec, &sample_cfg), TAG,
        "esp_codec_dev_open");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec, 75.0f), TAG,
        "esp_codec_dev_set_out_vol");
#ifdef USEADC
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_codec, 30.0f), TAG,
        "esp_codec_dev_set_in_gain");
#endif
    return ESP_OK;
}

esp_err_t espd_waveshare_s3_audio_init(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx, rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ESPD_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = ESPD_WAVESHARE_I2S_MCLK_GPIO,
            .bclk = PIN_BIT_CLOCK,
            .ws = PIN_WORD_SELECT,
            .dout = PIN_DATA_OUT,
            .din = PIN_DATA_IN,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*tx, &std_cfg), TAG,
        "i2s_channel_init_std_mode tx");
#ifdef USEADC
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*rx, &std_cfg), TAG,
        "i2s_channel_init_std_mode rx");
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*tx), TAG, "i2s_channel_enable tx");
#ifdef USEADC
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx), TAG, "i2s_channel_enable rx");
#endif

    ESP_RETURN_ON_ERROR(es8311_codec_init(*tx, *rx), TAG, "es8311_codec_init");
    espd_waveshare_usb_state_register_i2c_bus(s_i2c_bus);
    ESP_LOGI(TAG, "Waveshare S3 audio ready (%d Hz, ES8311)", ESPD_SAMPLE_RATE_HZ);
    return ESP_OK;
}
