/*
 * Waveshare ESP32-S3-AUDIO-Board: TCA9555 I/O expander (I2C) drives EXIO6/EXIO7
 * (Type-C D+/D− routing) and port1 lines for the NS4150 speaker amp (see board_profile.h).
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Program EXIO6/EXIO7 for USB data to the SoC (GPIO19/20) before TinyUSB. */
esp_err_t espd_waveshare_exio_apply_usb_mux(i2c_master_bus_handle_t bus);

/* Same as above using a short-lived I2C bus (before ES8311 creates the shared bus). */
esp_err_t espd_waveshare_exio_apply_usb_mux_ephemeral(void);

/**
 * Drive TCA9555/TCA9554 EXIO3 high (wiki: SD_D3/CS). Needed so the TF slot can use
 * SDMMC instead of SPI chip-select held low. Call after the shared I2C bus exists.
 */
esp_err_t espd_waveshare_exio_sd_cs_high(i2c_master_bus_handle_t bus);
