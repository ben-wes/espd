/*
 * Waveshare ESP32-S3-AUDIO-Board (AI Smart Speaker dev kit)
 * https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
 *
 * Build: ESPD_BOARD=waveshare_s3 (see boards/waveshare_s3/README.txt)
 */

#pragma once

#define PD_USE_WIFI
#define PD_USE_CONSOLE
#define PD_USE_SDCARD
#define PD_USE_ANALOG0
#define PD_INCLUDEPATCH
#define USEADC
#define IOCHANS 2

/*
 * When main.pd is loaded from SPIFFS (see espd_patch_store), skip bringing up
 * WiFi and the TCP/UDP patch transport so the board runs standalone. Set to 0
 * to keep WiFi + network patch loading even with a local main.pd.
 */
#ifndef ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK
#define ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK 1
#endif
/* Keep WiFi available for Pd patches, but don't run legacy espd net transport. */
#ifndef ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
#define ESPD_ENABLE_LEGACY_WIFI_TRANSPORT 0
#endif

/* I2S ↔ ES8311 (wiki “SPEAKER” / “MIC” tables; ESP is I2S master) */
#define PIN_BIT_CLOCK 13
#define PIN_WORD_SELECT 14
#define PIN_DATA_IN 15
#define PIN_DATA_OUT 16

#define ESPD_WAVESHARE_I2S_MCLK_GPIO 12
#define ESPD_WAVESHARE_I2C_SCL_GPIO 10
#define ESPD_WAVESHARE_I2C_SDA_GPIO 11

/*
 * Analog inputs sent as espd/ain/0..espd/ain/N (ADC1 only). Keep to
 * header-accessible pins.
 * We avoid GPIO10/11 here because they are already used as the shared I2C bus.
 * Default mapping:
 *   espd/ain/0 GPIO4
 *   espd/ain/1 GPIO5
 *   espd/ain/2 GPIO6
 *   espd/ain/3 GPIO7
 *
 * BAT_ADC on some board revisions is wired to GPIO1 via a solder option; if
 * you want battery sensing instead, set ESPD_ANALOG_PIN_0 to 1 and adjust count.
 */
#define ESPD_ANALOG_NUM_CHANNELS 4
#define ESPD_ANALOG_PIN_0 4
#define ESPD_ANALOG_PIN_1 5
#define ESPD_ANALOG_PIN_2 6
#define ESPD_ANALOG_PIN_3 7

/*
 * PWM-backed analog-style outputs via [s espd/aout/0], [s espd/aout/1], ... in Pd.
 * Default mapping uses header-accessible GPIO8 and GPIO9.
 */
#define ESPD_AOUT_NUM_CHANNELS 2
#define ESPD_AOUT_PIN_0 8
#define ESPD_AOUT_PIN_1 9
/* 12-bit LEDC limit on ESP32-S3 is 19.531 kHz max; keep below this to avoid fallback. */
#define ESPD_AOUT_PWM_FREQ_HZ 19531
#define ESPD_AOUT_PWM_FALLBACK_FREQ_HZ 10000

/*
 * TCA9555 (Waveshare schematic U4; wiki “TCA9555PWR”) on the same I2C bus as ES8311
 * (A2=L A1=H A0=L → 0x22 on v1.1; some builds strap 0x20 — firmware tries both).
 * EXIO6/EXIO7: USB switch. NS4150 amp enable is on TCA9555 port1 (EXIO8..15);
 * community defs use EXIO9; ESPHome used bit 8 — default mask enables both.
 */
#define ESPD_WAVESHARE_TCA9555_I2C_ADDR 0x22
#ifndef ESPD_WAVESHARE_TCA9555_I2C_ADDR_ALT
#define ESPD_WAVESHARE_TCA9555_I2C_ADDR_ALT 0x20
#endif
/* Legacy name used in early bring-up (chip is TCA9555, not TCA9554). */
#define ESPD_WAVESHARE_TCA9554_I2C_ADDR ESPD_WAVESHARE_TCA9555_I2C_ADDR

/* EXIO7 level that routes Type-C D+/D− to the SoC USB pins (GPIO19/20), not UART43/44. */
#ifndef ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL
#define ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL 1
#endif

/* EXIO6 = Camera_SEL on the USB switch; set per schematic if USB enumeration fails. */
#ifndef ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL
#define ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL 0
#endif

/*
 * TCA9555 port1 data bits (EXIO8..15) tied to NS4150 enable / strap lines.
 * 0 = leave port1 PA bits untouched. Default 0x03 = EXIO8+EXIO9 driven high.
 */
#ifndef ESPD_WAVESHARE_TCA9555_PA_PORT1_MASK
#define ESPD_WAVESHARE_TCA9555_PA_PORT1_MASK 0x03u
#endif
/* 1: OR mask onto OUT1 when enabling speaker path; 0: clear those bits (active-low amp). */
#ifndef ESPD_WAVESHARE_PA_PORT1_ACTIVE_HIGH
#define ESPD_WAVESHARE_PA_PORT1_ACTIVE_HIGH 1
#endif

/*
 * Boot-time “cable plugged” check: bring up TinyUSB CDC briefly and wait for host
 * enumeration (bounded). No cable / no host completes within the timeout (~500 ms).
 *
 * Default off: on ESP32-S3 the internal USB PHY is shared with USB Serial/JTAG.
 * A TinyUSB install/uninstall cycle at boot can leave macOS unable to re-enumerate
 * /dev/cu.usbmodem* until a full replug (IDFGH-15248 / IDFGH-8354). Set to 1 if you
 * need host-enumeration detection; firmware calls usb_new_phy(SERIAL_JTAG) after
 * teardown to improve handoff back to the built-in CDC.
 */
#ifndef ESPD_WAVESHARE_USB_BOOT_TINYUSB_PROBE
#define ESPD_WAVESHARE_USB_BOOT_TINYUSB_PROBE 0
#endif

#ifndef ESPD_WAVESHARE_USB_BOOT_HOST_WAIT_MS
#define ESPD_WAVESHARE_USB_BOOT_HOST_WAIT_MS 500
#endif

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
