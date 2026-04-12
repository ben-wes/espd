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

/*
 * Optional Type-C VBUS sense for USB-disk vs audio (see waveshare_s3_usb_state).
 * Set to the GPIO wired from VBUS (via divider or comparator) per schematic; -1
 * leaves the legacy behaviour (always start audio; no VBUS hotplug).
 */
#define ESPD_WAVESHARE_USB_VBUS_GPIO (-1)

/* 1 = GPIO high when USB VBUS is present (common with a resistor divider). */
#define ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH 1
