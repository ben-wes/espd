/*
 * Unified config.txt parser — single pass over the key=value file.
 * All subsystems read from g_espd_cfg after espd_config_load().
 */
#pragma once

#include <stdbool.h>

#define ESPD_CFG_MAX_PINS 8

/* USB MIDI mode from config.txt usb_midi_role= (read before USB stack starts).
 *   OFF (0, default): TinyUSB device — CDC + MSC only.
 *   DEVICE:           TinyUSB device — CDC + MSC + MIDI class.
 *   HOST:             USB-MIDI host — class-compliant controller on the OTG port.
 *                     Requires VBUS and ESPD_USE_USB_MIDI_HOST in the build. */
typedef enum {
    ESPD_USB_MIDI_OFF    = 0,
    ESPD_USB_MIDI_DEVICE = 1,
    ESPD_USB_MIDI_HOST   = 2,
} espd_usb_midi_mode_t;

typedef struct {
    espd_usb_midi_mode_t usb_midi_mode;

    /* WiFi */
    bool wifi_have_ssid;
    char wifi_ssid[33];
    char wifi_password[65];
    bool wifi_ap_have_ssid;
    char wifi_ap_ssid[33];
    char wifi_ap_password[65];
    int wifi_sync_ap_minutes; /* -1 = not set */

    /* Audio DMA / sample rate */
    int audio_dma_desc_num;   /* -1 = not set */
    int audio_dma_frame_num;  /* -1 = not set */
    int audio_sample_rate;    /* -1 = not set */

    /* AIN */
    bool ain_have_pins;
    int  ain_n;
    int  ain_pins[ESPD_CFG_MAX_PINS];
    int  ain_task_period_ms;  /* -1 = not set */
    int  ain_deadband;        /* -1 = not set */
    int  ain_report_every;    /* -1 = not set */

    /* AOUT */
    bool aout_have_pins;
    int  aout_n;
    int  aout_pins[ESPD_CFG_MAX_PINS];
    int  aout_pwm_freq_hz;    /* -1 = not set */

    /* DIN (GPIO, appended after BSP buttons) */
    bool din_have_pins;
    int  din_n;
    int  din_pins[ESPD_CFG_MAX_PINS];
    int  din_active_low;      /* -1 = not set */
    int  din_task_period_ms;  /* -1 = not set */

    /* DOUT */
    bool dout_have_pins;
    int  dout_n;
    int  dout_pins[ESPD_CFG_MAX_PINS];

    /* Touch */
    bool touch_have_pins;
    int  touch_n;
    int  touch_pins[ESPD_CFG_MAX_PINS];
    int  touch_task_period_ms;  /* -1 = not set */
    int  touch_report_every;    /* -1 = not set */
} espd_config_t;

extern espd_config_t g_espd_cfg;

void espd_config_load(void);
