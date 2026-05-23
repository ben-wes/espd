/*
 * Minimal ESPD I/O surface exposed to BSP espd_port.c integrations.
 */
#pragma once

#include "esp_err.h"

void espd_io_early_init(void);
void espd_io_board_init(void);
void espd_io_poll(void);
void espd_io_bind(void);
esp_err_t espd_io_sdcard_mount(void);

/** Forward a digital input change to Pd receivers (espd/din/N). */
void espd_din_changed(int idx, int pressed);
