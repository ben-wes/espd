/*
 * Waveshare ESP32-S3-AUDIO-Board: three push-buttons on the TCA9555
 * I/O expander (EXIO10/11/12 per schematic = port1 bits 2/3/4).
 *
 * The buttons are read over the shared I2C bus used by ES8311/TCA9555,
 * so espd_waveshare_s3_audio_init() must have run before init here.
 *
 * State is delivered to Pd as 0/1 floats on the symbols:
 *     [r espd/din/0]  (EXIO10 by default)
 *     [r espd/din/1]  (EXIO11 by default)
 *     [r espd/din/2]  (EXIO12 by default)
 *
 * Buttons are active-low on this board (pressed shorts to GND), so the
 * reported value is 1 while pressed and 0 while released.
 */

#pragma once

#include "esp_err.h"

/** Probe the TCA9555 on the shared I2C bus and enable button polling.
 *  Safe to call when no expander is present (logs a warning, becomes a no-op). */
esp_err_t espd_waveshare_s3_buttons_init(void);

