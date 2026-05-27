# Rapid patch dev (SD card + CDC)

Edit patches on the host, hear them on the board **almost immediately** for small
`.pd` / `config.txt` saves (typically well under a second after save). Use
microSD + `espd_sync.py` instead of copying every change through the slow
internal-flash USB drive.

Works on **macOS, Linux, and Windows** — Python 3 + `pyserial`, any CDC serial port.
Examples below use macOS device names; substitute `/dev/ttyACM0`, `COM3`, etc.

## Rules

| Storage | Role |
|---------|------|
| **microSD** (`/sdcard`) | Rapid dev when a card is **mounted** — `espd_sync.py` (default) |
| **Internal flash** (`/storage`) | Used only when SD is absent or not mounted |

Boot and CDC sync pick the store by **what is mounted** (SD → flash → SPIFFS), not
by which volume happens to contain `main.pd`. The host script follows the device
`PING` reply — no `--target` flag.

Insert a **microSD card** in the board. Put `main.pd` (and abstractions) on the card
via the sync script, not by pulling the card for each edit.

**Do not** open the internal flash volume in Finder while `espd_sync` is writing to
flash (it switches to **msc_sync** and hides the drive from the host). With no sync
tool running, USB **normal** mode shows the internal flash in Finder again.

While `espd_sync` is watching flash, the device stays in **msc_sync** (Finder
hidden). Quit the script and **power-cycle or hard-reset** the board to return to
**normal** mode and show the USB volume again. `msc_sync` is kept only across
`esp_restart()` from `MODE MSC_SYNC`, not across power-on or the reset button.
On connect the script does not force `msc_sync` until it actually PUTs files.

## Firmware / menuconfig

**ESPD Configuration → Storage & patches → USB device (OTG):**

| Option | Purpose |
|--------|---------|
| **Enable TinyUSB device stack** | OTG USB device + CDC serial (`print`, ESP_LOG on `cu.usbmodem*`) |
| **USB mass storage** | Internal-flash drive in Finder (default on; uncheck to omit) |
| **Patch sync over OTG CDC** | `espd_sync` PUT/RELOAD/PING — SD and/or internal flash MSC |

**Logs on OTG CDC:** `tinyusb_console_init` sends both Pd `print:` and `ESP_LOG`
(`I (…) tag:`) to `cu.usbmodem…`. Boot `ESP_LOG` is mostly gone by the time
`espd_sync` connects; you still see lines on **RELOAD**, **DTR connect**, etc.
**Do not flash on `cu.debug-console`.** On macOS it often appears beside
`cu.usbmodem*`, but with this OTG firmware esptool gets **no serial data** there
(`Failed to connect … No serial data received`). App logs and `espd_sync` use
**OTG CDC** (`cu.usbmodem*`) while the board is running.

**Reflash:** hold **BOOT**, tap **RESET**, release **BOOT** when esptool’s dots
connect, then `idf.py flash` (omit `-p` or use the port that responds — often
`cu.usbmodem*`, not `debug-console`). Auto-reset while the TinyUSB app is running
usually fails. After flash, **RESET** and sync on `cu.usbmodem*`.

USB product strings: **Component config → TinyUSB**.

Waveshare profiles default-enable OTG with MSC + patch sync. Uncheck **USB mass
storage** if you do not want the flash drive on the cable; uncheck **Patch sync**
if you only need serial, not `espd_sync`.

CDC commands (host → device):

- `PING` — check connection; replies `+OK PING sdcard mounted` or `sdcard not mounted`
- `PUT <relpath> <nbytes> <crc32hex>` — `relpath` may contain spaces (e.g.
  `no_voice/Tools/NH Tools 14.wav`); size and CRC are the last two fields. Device
  replies `+OK PUT skip` if SD already matches, else `+OK PUT ready` then raw bytes;
  verifies CRC (`+OK PUT done <crc>` or `-ERR PUT crc mismatch`)
- `RELOAD` — reload `main.pd` from `/sdcard` (audio loop, CPU1)
- `RESET` — reply `+OK RESET` then reboot the ESP

Replies are one line each: `+OK ...` or `-ERR ...` (also printed to stderr by the script).

## Host tool

```bash
pip install pyserial

idf.py build flash
# RESET; eject internal flash optional

# macOS (pick your OTG CDC port after ls /dev/cu.usb*)
python3 scripts/espd_sync.py -p /dev/cu.usbmodem1234561 ~/my_pd_project

# Linux example
# python3 scripts/espd_sync.py -p /dev/ttyACM0 ~/my_pd_project

# Windows example
# python3 scripts/espd_sync.py -p COM3 ~/my_pd_project
```

If several serial devices appear, pass the **OTG CDC** port explicitly (the one that
answers `PING` after a normal **RESET**). On macOS, `ls /dev/cu.usb*`; on Linux,
`ls /dev/ttyACM* /dev/ttyUSB*`.

On connect the script **syncs the whole project tree** (all subfolders; patches,
`config.txt`, and audio samples by default). Each file is one **`PUT`**: the device
**skips** if the target already has that size/CRC (`+OK PUT skip`), otherwise
receives bytes and **verifies after write** (retries on mismatch). Edit and save in
Pure Data; the watcher then `PUT`s changed `.pd` / `config.txt` only and sends
`RELOAD`. Use `--patches-only` to omit samples from sync/watch.

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' ~/my_pd_project
```

`--patches-only` — initial/resync sync without audio samples. `--no-initial-sync` —
watch for saves only (no connect sync). Large files have no firmware PUT cap;
CDC is still slow for multi‑MB uploads when content changed.

**CRC mismatch or disconnect during PUT:** usually not a “bad cable”. Causes have been
RX ring overflow (fixed: 16 KB buffer), **espd_dev task priority above TinyUSB** (fixed:
dev task now below USB), and host pacing for ~64 KB+ files (smaller chunks, longer
pause). `Device not configured` during `+OK PUT ready` means the board reset or USB
re-enumerated — reflash and retry; the script reconnects when possible.

Paths under subfolders and **names with spaces** are fine — no escaping; the firmware
parses the path between `PUT ` and the trailing `size crc` pair.

**Logs:** the script prints whatever arrives on the OTG port (mostly Pd `print:`).
`--no-esp-log` hides lines that look like `I/W/E (…) tag:` if any show up. Do not
run monitor and `espd_sync` on the same port at once.

| Stream | Pattern | Meaning |
|--------|---------|---------|
| stderr `→ …` | host → device | dev sync (PUT / PING / RELOAD) |
| stderr `← …` | device → host | `+OK` / `-ERR` replies (host adds `←`; wire is `+OK` only) |
| stderr `skip … (unchanged)` | host | `+OK PUT skip` from device |
| stderr `RELOAD done: …` / `RELOAD failed: …` | firmware status | patch reload finished (after `→ RELOAD` / `← +OK RELOAD pending`) |
| stdout `I/W/E (…) tag:` | ESP-IDF (rare on OTG) | usually on JTAG, not OTG |
| stdout *anything else* | Pd | `[print]`, `cpu:`, etc. |

With a TTY, `espd_sync.py` colorizes these (cyan/green/red dev, bright cyan `RELOAD done:`,
bright white Pd, dim ESP). Use `--no-color` or `--no-esp-log` to tone it down.

Quick check (after `idf.py flash` and board **RESET**):

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --ping ~/my_pd_project
```

On success, **stderr** shows `+OK PING sdcard mounted` then
`connected (...): +OK PING sdcard mounted`.
Stdout may keep printing `cpu:` — that is normal. If PING fails, reflash; do not use an old image without `espd_dev` CDC RX.

**Disconnect / reset:** By default the watcher waits for the CDC port and keeps
watching; it does **not** re-sync on reconnect until you opt in.
Use `--resync-on-reconnect` to run the same sync pass again after unplug/reset.
Use `--no-reconnect` to exit when the port goes away.

Remote reboot:

```bash
python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' --reset ~/my_pd_project
```

## Boot: where `main.pd` comes from

Firmware picks the **first** existing `main.pd` (same order for `config.txt`):

1. **microSD** — `/sdcard` (rapid dev; `espd_sync.py` writes here only)
2. **USB MSC volume** — `/storage` (Finder drag-and-drop; only if MSC is enabled)
3. **On-chip SPIFFS** — `/espd_pd` (`pdstore`; last resort — not host-accessible today)

If nothing matches, an optional **embedded test patch** (menuconfig **Embed fallback
test patch**) is compiled into the firmware — not SPIFFS.

`RELOAD` over CDC always reloads from `/sdcard` (needs a mounted card).

## Without USB OTG (fully off)

Disable **Enable TinyUSB device stack** to return to the pre-USB-device workflow:

- **Flash / monitor:** USB-Serial-JTAG (or UART) — usually one port, auto-reset on flash.
- **ESP_LOG:** IDF default console (not the CDC hook).
- **Patches:** SD, SPIFFS, etc. — not `espd_sync.py`.
