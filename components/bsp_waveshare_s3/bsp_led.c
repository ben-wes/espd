#include "bsp/bsp_led.h"
#include "bsp/waveshare_s3.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "bsp_led";

static led_strip_handle_t s_strip;
static TaskHandle_t s_refresh_task;
static volatile int s_dirty;

#ifndef BSP_LED_REFRESH_TASK_PRIO
#define BSP_LED_REFRESH_TASK_PRIO 2
#endif
#ifndef BSP_LED_REFRESH_TASK_CORE
#define BSP_LED_REFRESH_TASK_CORE 0
#endif
#ifndef BSP_LED_REFRESH_MIN_INTERVAL_MS
#define BSP_LED_REFRESH_MIN_INTERVAL_MS 10
#endif

static void bsp_led_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_dirty) {
            s_dirty = 0;
            led_strip_refresh(s_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(BSP_LED_REFRESH_MIN_INTERVAL_MS));
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

int bsp_led_count(void)
{
    return s_strip ? BSP_LED_COUNT : 0;
}

esp_err_t bsp_led_init(void)
{
    esp_err_t err;

    if (s_strip)
        return ESP_OK;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_LED_GPIO,
        .max_leds = BSP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    err = bsp_led_fill(BSP_LED_BOOT_R, BSP_LED_BOOT_G, BSP_LED_BOOT_B);
    if (err == ESP_OK)
        err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial paint failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WS2812 ring ready: %d LEDs on GPIO%d (boot %u,%u,%u)",
        (int)BSP_LED_COUNT, (int)BSP_LED_GPIO,
        (unsigned)BSP_LED_BOOT_R, (unsigned)BSP_LED_BOOT_G,
        (unsigned)BSP_LED_BOOT_B);

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(bsp_led_refresh_task,
            "led_refresh", 2048, NULL, BSP_LED_REFRESH_TASK_PRIO,
            &s_refresh_task, BSP_LED_REFRESH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create led_refresh task");
            s_refresh_task = NULL;
        }
    }
    return ESP_OK;
}

esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= BSP_LED_COUNT)
        return ESP_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_strip, (uint32_t)idx, r, g, b);
}

esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    int i;

    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    for (i = 0; i < BSP_LED_COUNT; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}

esp_err_t bsp_led_clear(void)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    return led_strip_clear(s_strip);
}

void bsp_led_mark_dirty(void)
{
    s_dirty = 1;
    if (s_refresh_task)
        xTaskNotifyGive(s_refresh_task);
}
