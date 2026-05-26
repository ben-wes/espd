# Rapid patch dev (SD card + CDC)

Save patches on the Mac, hear updates on the board in about **1–2 seconds**, without
using the internal-flash USB drive for every save.

## Rules

| Storage | Role |
|---------|------|
| **microSD** (`/sdcard`) | Rapid dev — `espd_sync.py` over CDC |
| **Internal flash** (`/storage`, MSC) | Bulk copy / no-SD fallback — Finder, slow |

Insert a **microSD card** in the board. Put `main.pd` (and abstractions) on the card
via the sync script, not by pulling the card for each edit.

**Do not** open the internal flash volume in Finder while syncing to SD — they are
independent; flash MSC can stay mounted for other files.

## Firmware / menuconfig

**ESPD Configuration → Storage & patches → USB device (OTG):**

| Option | Purpose |
|--------|---------|
| **Enable TinyUSB device stack** | OTG USB device — turn on first; sub-options appear below |
| **USB mass storage** | Internal-flash drive in Finder (default on; uncheck to omit) |
| **Serial on OTG CDC** | Pd `print` / `printf`, `espd_sync` on OTG `cu.usbmodem*` (default on) |
| **CDC rapid patch sync** | `espd_sync.py` PUT/RELOAD → `/sdcard` (needs SD) |
| **MSC volume label** | Finder disk name when MSC is enabled |

**Logs on OTG CDC:** `tinyusb_console_init` sends both Pd `print:` and `ESP_LOG`
(`I (…) tag:`) to `cu.usbmodem…`. Boot `ESP_LOG` is mostly gone by the time
`espd_sync` connects; you still see lines on **RELOAD**, **DTR connect**, etc.
`cu.debug-console` is for flash, not app logs with this firmware.
USB product strings: **Component config → TinyUSB**.

Waveshare profiles default-enable OTG with MSC + CDC + dev sync. Uncheck **USB mass
storage** only if you do not want the flash drive on the cable.

CDC commands (host → device):

- `PING` — check connection / SD presence
- `PUT <relpath> <nbytes>` then raw bytes — write under `/sdcard/`
- `RELOAD` — reload `main.pd` from `/sdcard` (audio loop, CPU1)
- `RESET` — reply `+OK RESET` then reboot the ESP

Replies are one line each: `+OK ...` or `-ERR ...` (also printed to stderr by the script).

## Host tool

```bash
pip install pyserial

idf.py build flash
# RESET; eject internal flash optional

python3 scripts/espd_sync.py -p /dev/cu.usbmodem1234561 ~/my_pd_project
```

If `ls /dev/cu.usb*` shows **more than one** `usbmodem` device, pass the **OTG CDC**
port explicitly (not USB-JTAG ROM). Flash uses `cu.debug-console`.

Edit and save in Pure Data on the Mac; the script watches the folder, `PUT`s changed
`.pd` / `config.txt` files, then sends `RELOAD`.

**Logs:** the script prints whatever arrives on the OTG port (mostly Pd `print:`).
`--no-esp-log` hides lines that look like `I/W/E (…) tag:` if any show up. Do not
run monitor and `espd_sync` on the same port at once.

| Stream | Pattern | Meaning |
|--------|---------|---------|
| stderr `→ …` | host → device | dev sync (PUT / PING / RELOAD) |
| stderr `← …` | device → host | `+OK` / `-ERR` replies (host adds `←`; wire is `+OK` only) |
| stderr `RELOAD: …` | firmware status | patch reload done (after `→ RELOAD` / `← +OK RELOAD pending`) |
| stdout `I/W/E (…) tag:` | ESP-IDF (rare on OTG) | usually on JTAG, not OTG |
| stdout *anything else* | Pd | `[print]`, `cpu:`, etc. |

With a TTY, `espd_sync.py` colorizes these (cyan/green/red dev, bright cyan `RELOAD:`,
bright white Pd, dim ESP). Use `--no-color` or `--no-esp-log` to tone it down.

Quick check (after `idf.py flash` and board **RESET**):

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --ping ~/my_pd_project
```

On success, **stderr** shows `+OK PING sdcard` then `connected (...): +OK PING sdcard`.
Stdout may keep printing `cpu:` — that is normal. If PING fails, reflash; do not use an old image without `espd_dev` CDC RX.

**Disconnect / reset:** By default the watcher waits for the CDC port to return (after
unplug, `RESET`, or the reset button) and re-syncs all patch files. Use `--no-reconnect`
to exit instead.

Remote reboot:

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --reset ~/my_pd_project
```

## Boot probe order (unchanged)

SD → SPIFFS → internal `/storage`. With SD present and `main.pd` on the card, Pd
loads from `/sdcard` at boot.

## Without SD

Use the **USB flash drive** (MSC) for patches — no rapid CDC path. See
[USB_MSC_AND_AUDIO.md](USB_MSC_AND_AUDIO.md).

## Without the USB flash drive

Enable **TinyUSB on OTG** (CDC and dev sync default on). Uncheck **USB mass storage** only.
`espd_sync.py` still works; patches on **microSD**. Flash via **USB-JTAG**
(`cu.debug-console`), **RESET**, then sync on `cu.usbmodem*`.

## Without USB OTG (fully off)

Disable **Enable TinyUSB device stack** to return to the pre-USB-device workflow:

- **Flash / monitor:** USB-Serial-JTAG (or UART) — usually one port, auto-reset on flash.
- **ESP_LOG:** IDF default console (not the CDC hook).
- **Patches:** SD, SPIFFS, etc. — not `espd_sync.py`.
