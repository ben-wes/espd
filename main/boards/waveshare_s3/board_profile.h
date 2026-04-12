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
 * VBUS source for USB-disk vs audio (see waveshare_s3_usb_state):
 *   NONE — default. Right choice for stock board + battery only (no add-ons);
 *           no I2C device at 0x2D on this PCB, and the audio wiki does not name a
 *           VBUS GPIO — keep NONE until the schematic gives a sense pin, if any.
 *   GPIO — ESPD_WAVESHARE_USB_VBUS_GPIO >= 0, divider/comparator from Type-C VBUS.
 *   UPS_HAT_E — optional separate Waveshare UPS HAT (E) stacked on the same I2C
 *               as ES8311 (addr 0x2D); not part of the bare S3-Audio board.
 */
#define ESPD_WAVESHARE_VBUS_BACKEND_NONE 0
#define ESPD_WAVESHARE_VBUS_BACKEND_GPIO 1
#define ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E 2
#define ESPD_WAVESHARE_VBUS_BACKEND ESPD_WAVESHARE_VBUS_BACKEND_NONE

#define ESPD_WAVESHARE_UPS_HAT_I2C_ADDR 0x2D
#define ESPD_WAVESHARE_UPS_HAT_REG_CHARGING 0x02
#define ESPD_WAVESHARE_UPS_HAT_VBUS_BIT 5

/*
 * Used when ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_GPIO.
 * -1 = N/A (ignored unless backend is GPIO).
 */
#define ESPD_WAVESHARE_USB_VBUS_GPIO (-1)

/* 1 = GPIO high when USB VBUS is present (common with a resistor divider). */
#define ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH 1
