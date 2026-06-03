/*
 * Unified config.txt parser — single pass over the key=value file.
 * All subsystems read from g_espd_cfg after espd_config_load().
 */
#pragma once

#include <stdbool.h>

#define ESPD_CFG_MAX_PINS 8

typedef struct {
    /* WiFi */
    bool wifi_have_ssid;
    char wifi_ssid[33];
    char wifi_password[65];

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
