#include "boards/waveshare_s3/board_profile.h"
#include "boards/waveshare_s3/waveshare_s3_leds.h"

#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waveshare_leds";

static led_strip_handle_t s_strip;
static TaskHandle_t s_refresh_task;
static volatile int s_dirty;

#ifndef ESPD_LED_REFRESH_TASK_PRIO
#define ESPD_LED_REFRESH_TASK_PRIO 2  /* well below audio task */
#endif
#ifndef ESPD_LED_REFRESH_TASK_CORE
#define ESPD_LED_REFRESH_TASK_CORE 0  /* opposite core from Pd audio (core 1) */
#endif
#ifndef ESPD_LED_REFRESH_MIN_INTERVAL_MS
#define ESPD_LED_REFRESH_MIN_INTERVAL_MS 10  /* coalesce bursts; ~100 fps cap */
#endif

static void espd_leds_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wait for a dirty signal. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Coalesce: drop further wake-ups arriving during the cool-down. */
        if (s_dirty) {
            s_dirty = 0;
            led_strip_refresh(s_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(ESPD_LED_REFRESH_MIN_INTERVAL_MS));
        /* If new updates landed during the cool-down, eat any pending notify
         * so we run exactly once more without unbounded queueing. */
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

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

    /* Spawn the dedicated refresh task so RMT writes never run on the audio
     * thread. Pinned to the opposite core to avoid sharing CPU with senddacs(). */
    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_leds_refresh_task,
            "led_refresh", 2048, NULL, ESPD_LED_REFRESH_TASK_PRIO,
            &s_refresh_task, ESPD_LED_REFRESH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create led_refresh task");
            s_refresh_task = NULL;
        }
    }
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

void espd_waveshare_s3_leds_mark_dirty(void)
{
    s_dirty = 1;
    if (s_refresh_task)
        xTaskNotifyGive(s_refresh_task);
}