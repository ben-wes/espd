/*
 * SPDX-FileCopyrightText: 2025 ESPD contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * BSP for Waveshare ESP32-S3-AUDIO-Board (AI Smart Speaker dev kit).
 * https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
 *
 * API shape follows espressif/esp-bsp so this component can be upstreamed.
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 * Board identity
 **************************************************************************************************/
#define BSP_BOARD_WAVESHARE_S3

/**************************************************************************************************
 * Capabilities
 **************************************************************************************************/
#define BSP_CAPS_AUDIO           1
#define BSP_CAPS_AUDIO_SPEAKER   1
#define BSP_CAPS_AUDIO_MIC       1
#define BSP_CAPS_BUTTONS         1
#define BSP_CAPS_LED             1
#define BSP_CAPS_SDCARD          1
#define BSP_CAPS_DISPLAY         0
#define BSP_CAPS_TOUCH           0
#define BSP_CAPS_IMU             0

/**************************************************************************************************
 * Pinout
 **************************************************************************************************/
#define BSP_I2C_SCL              (GPIO_NUM_10)
#define BSP_I2C_SDA              (GPIO_NUM_11)

#define BSP_I2S_MCLK             (GPIO_NUM_12)
#define BSP_I2S_SCLK             (GPIO_NUM_13)
#define BSP_I2S_LCLK             (GPIO_NUM_14)
#define BSP_I2S_DOUT             (GPIO_NUM_16)
#define BSP_I2S_DSIN             (GPIO_NUM_15)

#define BSP_LED_GPIO             (GPIO_NUM_38)
#define BSP_LED_COUNT            7

#define BSP_SD_CLK               (GPIO_NUM_40)
#define BSP_SD_CMD               (GPIO_NUM_42)
#define BSP_SD_D0                (GPIO_NUM_41)

/** TCA9555 on I2C (primary address verified on this board). */
#define BSP_IO_EXPANDER_I2C_ADDR       0x20
#define BSP_IO_EXPANDER_I2C_ADDR_ALT   0x22

/** On-board push buttons on TCA9555 port1 (active low). */
#define BSP_BUTTON_COUNT               3
#define BSP_BUTTON0_PORT1_BIT          1  /* EXIO9 */
#define BSP_BUTTON1_PORT1_BIT          2  /* EXIO10 */
#define BSP_BUTTON2_PORT1_BIT          3  /* EXIO11 */

/** WS2812 boot indicator color (RGB 0..255). */
#define BSP_LED_BOOT_R                 0
#define BSP_LED_BOOT_G                 0
#define BSP_LED_BOOT_B                 0

/** EXIO7 level routing Type-C D+/D- to SoC USB (GPIO19/20). */
#ifndef CONFIG_BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL
#define CONFIG_BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL 1
#endif
#define BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL CONFIG_BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL

/** EXIO6 = Camera_SEL on the USB switch. */
#ifndef CONFIG_BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL
#define CONFIG_BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL 0
#endif
#define BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL CONFIG_BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL

/** TCA9555 port1 bits (EXIO8..15) for NS4150 speaker amp enable. */
#ifndef CONFIG_BSP_WAVESHARE_PA_PORT1_MASK
#define CONFIG_BSP_WAVESHARE_PA_PORT1_MASK 0x03
#endif
#define BSP_WAVESHARE_PA_PORT1_MASK ((uint8_t)CONFIG_BSP_WAVESHARE_PA_PORT1_MASK)

#ifndef CONFIG_BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH
#define CONFIG_BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH 1
#endif
#define BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH CONFIG_BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH

/**************************************************************************************************
 * I2C
 **************************************************************************************************/

/** Initialize the shared I2C master bus (idempotent). */
esp_err_t bsp_i2c_init(void);

/** Shared I2C bus used by ES8311, ES7210, and TCA9555. NULL before init. */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/**************************************************************************************************
 * I/O expander (TCA9555)
 **************************************************************************************************/

/** Program EXIO6/EXIO7 USB mux and speaker PA lines on the TCA9555. */
esp_err_t bsp_waveshare_io_expander_apply(void);

/** Drive EXIO3 high so the TF slot can use SDMMC (not SPI CS held low). */
esp_err_t bsp_waveshare_io_expander_sd_cs_high(void);

/**************************************************************************************************
 * Audio
 **************************************************************************************************/

#define BSP_AUDIO_SAMPLE_RATE_HZ 48000

/**
 * Initialize I2S and ES8311 playback path (and ES7210 capture when enabled in Kconfig).
 * After success, use bsp_audio_codec_speaker_init() / microphone_init() for handles.
 */
esp_err_t bsp_audio_init(void);

/** Speaker codec handle. NULL before bsp_audio_init(). */
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

#if BSP_CAPS_AUDIO_MIC
/** Microphone codec handle. NULL if capture unavailable or disabled in Kconfig. */
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
#endif

#ifdef __cplusplus
}
#endif
