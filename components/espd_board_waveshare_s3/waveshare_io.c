/*
 * ESPD ↔ esp-bsp adapter for Waveshare ESP32-S3-AUDIO (I/O only).
 *
 * Implements espd_integration bsp_io.h on top of esp-bsp (LED strip, iot_button).
 * SD mount via espd_bsp_sdcard_mount(); audio via espd_bsp_audio_hw_init().
 */

#include "bsp/esp-bsp.h"
#include "espd_bsp_sdcard.h"

#define ESPD_BSP_IO_NO_SDCARD_DECL
#include "bsp/bsp_io.h"
#undef ESPD_BSP_IO_NO_SDCARD_DECL

#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "led_strip.h"

static const char *TAG = "espd_board_waveshare";

#define ESPD_WAVESHARE_BUTTON_COUNT 3

static const bsp_button_t s_button_map[ESPD_WAVESHARE_BUTTON_COUNT] = {
    BSP_BUTTON_VOLUP,
    BSP_BUTTON_PLAY,
    BSP_BUTTON_VOLDOWN,
};

static button_handle_t s_buttons[BSP_BUTTON_NUM];
static bool s_buttons_ready;
static led_strip_handle_t s_strip;
static TaskHandle_t s_refresh_task;
static volatile int s_led_dirty;
static void (*s_button_handler)(int idx, int pressed);

#ifndef ESPD_LED_REFRESH_TASK_PRIO
#define ESPD_LED_REFRESH_TASK_PRIO 2
#endif
#ifndef ESPD_LED_REFRESH_TASK_CORE
#define ESPD_LED_REFRESH_TASK_CORE 0
#endif
#ifndef ESPD_LED_REFRESH_MIN_INTERVAL_MS
#define ESPD_LED_REFRESH_MIN_INTERVAL_MS 10
#endif

static void espd_bsp_led_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_led_dirty) {
            s_led_dirty = 0;
            led_strip_refresh(s_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(ESPD_LED_REFRESH_MIN_INTERVAL_MS));
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

static void espd_bsp_button_event(void *button_handle, void *usr_data)
{
    int map_idx = (int)(intptr_t)usr_data;
    button_event_t event = iot_button_get_event(button_handle);

    if (!s_button_handler || map_idx < 0 || map_idx >= ESPD_WAVESHARE_BUTTON_COUNT)
        return;

    if (event == BUTTON_PRESS_DOWN)
        s_button_handler(map_idx, 1);
    else if (event == BUTTON_PRESS_UP)
        s_button_handler(map_idx, 0);
}

int bsp_led_count(void)
{
    return s_strip ? BSP_LED_STRIP_NUM : 0;
}

esp_err_t bsp_led_init(void)
{
    esp_err_t err;

    if (s_strip)
        return ESP_OK;

    err = bsp_led_strip_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_led_strip_init: %s", esp_err_to_name(err));
        return err;
    }

    s_strip = bsp_led_strip_get_handle();
    if (!s_strip) {
        ESP_LOGW(TAG, "no LED strip handle");
        return ESP_FAIL;
    }

    err = bsp_led_fill(0, 0, 0);
    if (err == ESP_OK)
        err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial LED paint: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WS2812 ready: %d LEDs on GPIO%d", BSP_LED_STRIP_NUM,
        (int)BSP_LED_STRIP_IO);

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_bsp_led_refresh_task,
            "led_refresh", 2048, NULL, ESPD_LED_REFRESH_TASK_PRIO,
            &s_refresh_task, ESPD_LED_REFRESH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "led_refresh task create failed");
            s_refresh_task = NULL;
        }
    }
    return ESP_OK;
}

esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= BSP_LED_STRIP_NUM)
        return ESP_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_strip, (uint32_t)idx, r, g, b);
}

esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    int i;

    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    for (i = 0; i < BSP_LED_STRIP_NUM; i++) {
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
    s_led_dirty = 1;
    if (s_refresh_task)
        xTaskNotifyGive(s_refresh_task);
}

int bsp_button_count(void)
{
    return s_buttons_ready ? ESPD_WAVESHARE_BUTTON_COUNT : 0;
}

esp_err_t bsp_button_init(void)
{
    int btn_cnt = 0;
    esp_err_t err;

    if (s_buttons_ready)
        return ESP_OK;

    err = bsp_iot_button_create(s_buttons, &btn_cnt, BSP_BUTTON_NUM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_iot_button_create: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < ESPD_WAVESHARE_BUTTON_COUNT; i++) {
        bsp_button_t b = s_button_map[i];
        button_handle_t btn = s_buttons[b];
        if (!btn)
            continue;
#if BUTTON_VER_MAJOR >= 4
        iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL,
            espd_bsp_button_event, (void *)(intptr_t)i);
        iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL,
            espd_bsp_button_event, (void *)(intptr_t)i);
#else
        iot_button_register_cb(btn, BUTTON_PRESS_DOWN,
            espd_bsp_button_event, (void *)(intptr_t)i);
        iot_button_register_cb(btn, BUTTON_PRESS_UP,
            espd_bsp_button_event, (void *)(intptr_t)i);
#endif
    }

    s_buttons_ready = true;
    ESP_LOGI(TAG, "buttons ready (K1/K2/K3 → espd/din/0..2)");
    return ESP_OK;
}

void bsp_button_set_handler(void (*handler)(int idx, int pressed))
{
    s_button_handler = handler;
}

void bsp_button_poll(void)
{
    /* iot_button delivers events via callbacks; poll is a no-op. */
}

esp_err_t espd_bsp_sdcard_mount(const char *mount_point)
{
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;

    if (mount_point && strcmp(mount_point, BSP_SD_MOUNT_POINT) != 0)
        ESP_LOGW(TAG, "mount_point %s ignored (BSP uses %s)",
            mount_point, BSP_SD_MOUNT_POINT);

    esp_io_expander_handle_t exp = bsp_io_expander_init();

    if (exp) {
        esp_io_expander_set_dir(exp, BSP_SD_DET, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, BSP_SD_DET, 1);
    }
    {
        bsp_sdcard_cfg_t cfg = {0};
        return bsp_sdcard_sdmmc_mount(&cfg);
    }
}
