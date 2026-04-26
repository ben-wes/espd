#include "boards/waveshare_s3/board_profile.h"
#include "boards/waveshare_s3/waveshare_s3_leds.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "waveshare_leds";

static led_strip_handle_t s_strip;

esp_err_t espd_waveshare_s3_leds_init(void)
{
    if (s_strip)
        return ESP_OK;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = ESPD_WAVESHARE_LED_GPIO,
        .max_leds = ESPD_WAVESHARE_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        /* Use RGB component format: led_strip v3 treats the set_pixel arg
         * order as matching this format, so with RGB we can keep our API as
         * (r, g, b) while the driver still sends the correct WS2812 wire order. */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz: standard for WS2812 timing. */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    /* Paint the configured boot color on every pixel so we can verify hardware. */
    err = espd_waveshare_s3_leds_fill(
        ESPD_WAVESHARE_LED_BOOT_R,
        ESPD_WAVESHARE_LED_BOOT_G,
        ESPD_WAVESHARE_LED_BOOT_B);
    if (err == ESP_OK)
        err = espd_waveshare_s3_leds_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial paint failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WS2812 ring ready: %d LEDs on GPIO%d (boot color %u,%u,%u)",
        (int)ESPD_WAVESHARE_LED_COUNT, (int)ESPD_WAVESHARE_LED_GPIO,
        (unsigned)ESPD_WAVESHARE_LED_BOOT_R,
        (unsigned)ESPD_WAVESHARE_LED_BOOT_G,
        (unsigned)ESPD_WAVESHARE_LED_BOOT_B);
    return ESP_OK;
}

esp_err_t espd_waveshare_s3_leds_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= (int)ESPD_WAVESHARE_LED_COUNT)
        return ESP_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_strip, (uint32_t)idx, r, g, b);
}

esp_err_t espd_waveshare_s3_leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    int i;
    for (i = 0; i < (int)ESPD_WAVESHARE_LED_COUNT; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}

esp_err_t espd_waveshare_s3_leds_refresh(void)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    return led_strip_refresh(s_strip);
}

esp_err_t espd_waveshare_s3_leds_clear(void)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    return led_strip_clear(s_strip);
}
