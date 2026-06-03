/*
 * ESPD I/O: PureData Binding and Routing Layer.
 *
 * This module serves as the event-routing bridge between PureData and physical I/O:
 * 1. Binds Pd receivers to capture LED color/clear commands from Pd (espd/led, espd/led/N).
 * 2. Routes button presses and digital inputs to Pd receiver targets (espd/din/N).
 * 3. Coordinates the periodic polling of all system inputs.
 */
#pragma once

void espd_pd_io_poll(void);
void espd_pd_io_bind_leds(void);

/** Forward a digital input change to Pd receivers (espd/din/N). */
void espd_din_changed(int idx, int pressed);
void espd_din_gpio_poll(void);
void espd_din_log_map(void);
