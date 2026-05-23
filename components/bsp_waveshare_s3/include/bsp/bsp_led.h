#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t bsp_led_init(void);
esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_clear(void);
void bsp_led_mark_dirty(void);
int bsp_led_count(void);
