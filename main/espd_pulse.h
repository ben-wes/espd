/*
 * Hardware-timed GPIO pulse trains (RMT) — independent of Pd sample rate.
 *
 * Channels map from config.txt pulse_pins= to [espdpulse N].
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESPD_PULSE_MAX_CHANNELS 4

void espd_pulse_init(void);
bool espd_pulse_channel_ready(int ch);

/* Update timing (takes effect on next pulses, or immediately if continuous). */
void espd_pulse_set_rate(int ch, float hz);
void espd_pulse_set_duty(int ch, float duty);

/* pulses: N>0 finite burst, -1 continuous, 0 stop. */
void espd_pulse_pulses(int ch, int32_t n);
