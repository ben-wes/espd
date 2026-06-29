/*
 * Pd <-> USB MIDI bridge.
 *
 * Implements Pd's platform MIDI backend (sys_do_open_midi / sys_putmidimess /
 * sys_putmidibyte / sys_poll_midi / sys_close_midi / midi_getdevs) on top of
 * TinyUSB's MIDI device class, so native Pd objects ([notein], [ctlin],
 * [noteout], [midiin], ...) talk to a USB MIDI port that appears on the host.
 *
 * Serial monitoring (CDC) is a separate USB interface and is unaffected.
 */
#pragma once

/* True when config.txt sets usb_midi_role=device or host (MIDI transport active). */
bool espd_midi_runtime_active(void);

/* Reset the MIDI timing queues and open the (single) USB MIDI in/out device.
 * Call once after pd_init() when CONFIG_ESPD_USE_USB_MIDI is enabled. No-op when
 * usb_midi_role is omitted (ESPD_USB_MIDI_OFF). */
void espd_midi_init(void);

/* Drain inbound USB MIDI into Pd and flush Pd's outbound MIDI queue. Call once
 * per scheduler tick (block rate). No-op when usb_midi_role is omitted. */
void espd_midi_poll(void);

/* ── Generic MIDI source/sink, shared by the device (TinyUSB) and host
 *    (USB-MIDI host) backends. Both feed the same Pd MIDI core. ── */

/* Push raw MIDI bytes (already de-packetized) toward Pd. Cross-task safe;
 * drained in espd_midi_poll/sys_poll_midi. Used by the USB-MIDI host driver. */
void espd_midi_inject_rx(const unsigned char *data, unsigned len);

/* Outbound writer: when set (non-NULL), Pd's outgoing MIDI bytes are handed to
 * this callback instead of the TinyUSB device class. The USB-MIDI host driver
 * registers one to forward Pd output to a connected controller. Pass NULL to
 * restore the default device-class path. */
typedef void (*espd_midi_out_fn)(const unsigned char *data, unsigned len);
void espd_midi_set_output(espd_midi_out_fn fn);
