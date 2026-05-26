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

**ESPD Configuration → Storage & patches → USB composite (OTG):**

| Option | Purpose |
|--------|---------|
| **Enable USB MSC + CDC** | Flash drive + serial on one cable |
| **Route logs to CDC** | `cu.usbmodem*` for printf / ESP_LOG |
| **CDC rapid patch sync** | `espd_sync.py` PUT/RELOAD to `/sdcard` (needs SD) |
| **MSC volume label** | Finder disk name for internal flash (max 11 chars, default `ESPD`) |

USB interface strings (manufacturer/product) are under **Component config → TinyUSB**
if you want to change those too.

Waveshare board profiles enable dev sync by default when SD + USB composite are on.

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

python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' ~/my_pd_project
```

Edit and save in Pure Data on the Mac; the script watches the folder, `PUT`s changed
`.pd` / `config.txt` files, then sends `RELOAD`.

**Logs:** the script tails everything on one CDC port. Do not run `idf.py monitor` on
the same port simultaneously.

| Stream | Pattern | Meaning |
|--------|---------|---------|
| stderr `→ …` | host command | dev sync (PUT / PING / RELOAD) |
| stderr `+OK` / `-ERR` | device reply | dev protocol |
| stdout `cpu:` | Pd load meter | patch DSP usage |
| stdout `I/W/E (…) tag:` | ESP-IDF | storage, WiFi, USB init |

With a TTY, `espd_sync.py` colorizes these (cyan/green/red dev, yellow Pd, dim ESP).
Use `--no-color` or `--no-esp-log` to tone it down. There is no binary framing — only
line prefixes distinguish dev vs Pd vs ESP on the shared serial stream.

Quick check (after `idf.py flash` and board **RESET**):

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --ping ~/my_pd_project
```

On success, **stderr** shows `+OK PING sdcard` then `connected (...): +OK PING sdcard`.
Stdout may keep printing `cpu:` — that is normal. If PING fails, reflash; do not use an old image without `espd_dev` CDC RX.

**Disconnect / reset:** By default the watcher waits for the CDC port to return (after
unplug, `RESET`, or the reset button) and re-syncs all patch files. Use `--no-reconnect`
to exit instead. Remote reboot:

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --reset ~/my_pd_project
```

## Boot probe order (unchanged)

SD → SPIFFS → internal `/storage`. With SD present and `main.pd` on the card, Pd
loads from `/sdcard` at boot.

## Without SD

Use the **USB flash drive** (MSC) for patches — no rapid CDC path. See
[USB_MSC_AND_AUDIO.md](USB_MSC_AND_AUDIO.md).
