#pragma once

#include "esp_err.h"

esp_err_t bsp_button_init(void);
void bsp_button_set_handler(void (*handler)(int idx, int pressed));
void bsp_button_poll(void);
