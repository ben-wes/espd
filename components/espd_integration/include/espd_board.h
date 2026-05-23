/*
 * Board hooks used by espd.c / espd_io.c (implemented in main/espd_board.c).
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifndef ESPD_SDCARD_MOUNT
#define ESPD_SDCARD_MOUNT "/sdcard"
#endif

esp_err_t espd_board_early_init(void);
esp_err_t espd_board_init(void);
void espd_board_poll(void);
esp_err_t espd_board_sdcard_mount(void);

int espd_board_led_count(void);
esp_err_t espd_board_led_set(int idx, uint8_t r, uint8_t g, uint8_t b);
esp_err_t espd_board_led_fill(uint8_t r, uint8_t g, uint8_t b);
esp_err_t espd_board_led_clear(void);
void espd_board_led_mark_dirty(void);
