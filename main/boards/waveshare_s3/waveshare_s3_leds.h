/*
 * Waveshare ESP32-S3-AUDIO-Board: on-board WS2812 ring (7 RGB LEDs)
 * connected to GPIO38. Driven via the espressif/led_strip managed component.
 *
 * Step 1 (this file): bring-up — at boot all LEDs are filled with the default
 * color defined by ESPD_WAVESHARE_LED_BOOT_{R,G,B}.
 *
 * Step 2 (later): expose Pd messages so a patch can set per-LED colors.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

/** Initialize the LED strip and paint the configured boot color on every pixel. */
esp_err_t espd_waveshare_s3_leds_init(void);

/** Set a single LED (0..count-1) to the given 8-bit RGB color. */
esp_err_t espd_waveshare_s3_leds_set(int idx, uint8_t r, uint8_t g, uint8_t b);

/** Fill the whole ring with one color. */
esp_err_t espd_waveshare_s3_leds_fill(uint8_t r, uint8_t g, uint8_t b);

/** Push the staged colors to the strip. set/fill stage values; refresh latches them. */
esp_err_t espd_waveshare_s3_leds_refresh(void);

/** Turn every LED off (equivalent to fill(0,0,0) + refresh). */
esp_err_t espd_waveshare_s3_leds_clear(void);

/** Mark the strip as needing a refresh; safe to call from the audio thread. */
void espd_waveshare_s3_leds_mark_dirty(void);

/** Refresh the strip if dirty; call from main loop, throttled. */
void espd_waveshare_s3_leds_poll(void);
