/*
 * GPIO Peripherals: Hardware Initialization and Drivers.
 *
 * This module manages low-level ESP32-S3 driver initialization and data acquisition
 * for user-configured pins specified in config.txt (via g_espd_cfg):
 * - AIN (ADC Oneshot)
 * - AOUT (LEDC PWM)
 * - DIN (Raw GPIO input tasks with debounce)
 * - DOUT (Raw GPIO output level setters)
 * - Touch (Capacitive Touch sensor FSM)
 */
#pragma once

void espd_aout_init(void);
void espd_dout_init(void);
void espd_din_gpio_init(void);
void espd_ain_init(void);
void espd_touch_init(void);

void espd_ain_poll(void);
void espd_touch_poll(void);

void espd_din_gpio_poll(void);
void espd_din_log_map(void);
