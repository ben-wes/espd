/*
 * Generic board bring-up: probe optional bsp_* peripherals and use what exists.
 *
 * Managed esp-bsp boards use espd_bsp_shim for I/O and SD card.
 */

#include "espd_board.h"
#include "espd_io.h"

#include "bsp/bsp_io.h"

#include "esp_log.h"

#if CONFIG_ESPD_BOARD_WAVESHARE_S3
#include "espd_bsp_shim.h"
#endif

static const char *TAG = "espd_board";

static void espd_board_log_optional(const char *what, esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
}

esp_err_t espd_board_early_init(void)
{
    espd_board_log_optional("LED init", bsp_led_init());
    return ESP_OK;
}

esp_err_t espd_board_init(void)
{
    bsp_button_set_handler(espd_din_changed);
    espd_board_log_optional("buttons", bsp_button_init());
    return ESP_OK;
}

void espd_board_poll(void)
{
    bsp_button_poll();
}

esp_err_t espd_board_sdcard_mount(void)
{
#if CONFIG_ESPD_BOARD_WAVESHARE_S3
    return espd_bsp_waveshare_sdcard_mount();
#else
    return bsp_sdcard_mount(ESPD_SDCARD_MOUNT);
#endif
}

int espd_board_led_count(void)
{
    return bsp_led_count();
}

esp_err_t espd_board_led_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    return bsp_led_set(idx, r, g, b);
}

esp_err_t espd_board_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    return bsp_led_fill(r, g, b);
}

esp_err_t espd_board_led_clear(void)
{
    return bsp_led_clear();
}

void espd_board_led_mark_dirty(void)
{
    bsp_led_mark_dirty();
}
