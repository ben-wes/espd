/*
 * Waveshare ESP32-S3-AUDIO-Board (AI Smart Speaker dev kit)
 * https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
 *
 * Build: ESPD_BOARD=waveshare_s3 (see boards/waveshare_s3/README.txt)
 */

#pragma once

#define PD_USE_WIFI
#define PD_USE_CONSOLE
#define PD_INCLUDEPATCH
#define USEADC
#define IOCHANS 2

/* I2S ↔ ES8311 (wiki “SPEAKER” / “MIC” tables; ESP is I2S master) */
#define PIN_BIT_CLOCK 13
#define PIN_WORD_SELECT 14
#define PIN_DATA_IN 15
#define PIN_DATA_OUT 16

#define ESPD_WAVESHARE_I2S_MCLK_GPIO 12
#define ESPD_WAVESHARE_I2C_SCL_GPIO 10
#define ESPD_WAVESHARE_I2C_SDA_GPIO 11
