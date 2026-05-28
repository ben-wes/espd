# Rapid patch dev (CDC sync)

Edit patches on the host, hear them on the board **almost immediately** for small
`.pd` / `config.txt` saves. Uses `espd_sync.py` over OTG CDC instead of slow
drag-and-drop to the internal-flash USB volume for every change.

General build and storage rules: **[README.md](../README.md)**.

Works on **macOS, Linux, and Windows** — Python 3 + [pyserial](https://pyserial.readthedocs.io/).

## Active store

Boot and CDC sync use the same rule as firmware storage ([espd_storage.c](../main/espd_storage.c)):

| Mounted | Path | Role |
|---------|------|------|
| SD card | `/sdcard` | Preferred when present |
| Internal flash | `/storage` | When SD is absent and MSC partition is mounted |

The host script does **not** pick the store itself. On connect it sends `STATUS` and syncs to whatever the device reports (`sdcard=yes` → SD, else `internal=yes` → flash with **msc_sync**). If storage changes (e.g. SD inserted later), the script updates the target and can resync.

**Do not** open the internal flash volume in Finder while `espd_sync` is writing to flash (device enters **msc_sync** and hides the drive). Quit the script and reset the board to return to **normal** mode and show the USB volume again.

## Firmware / menuconfig

**ESPD Configuration → Storage & patches → USB device (OTG):**

| Option | Purpose |
|--------|---------|
| **Enable TinyUSB device stack** | OTG USB + CDC serial |
| **USB mass storage** | Internal-flash drive on the cable |
| **Patch sync over OTG CDC** | `espd_sync` PUT / RELOAD / STATUS |

Waveshare S3 profile enables OTG, MSC, and patch sync by default.

### Flashing

Hold **BOOT**, tap **RESET**, release **BOOT** when esptool connects, then `idf.py flash`. Auto-reset while the TinyUSB app is running often fails.

On macOS, **do not flash on `cu.debug-console`** — use the OTG CDC port (`cu.usbmodem*`) that answers `STATUS` after a normal reset. See esptool output if unsure.

USB product strings: **Component config → TinyUSB**.

## CDC protocol

Host → device:

- `STATUS` — `+OK STATUS sdcard=yes|no internal=yes|no mode=normal|msc_sync` (`internal` = `/storage` mounted on the **device** for I/O, not whether Finder shows a disk)
- `PUT <relpath> <nbytes> <crc32hex>` — path may contain spaces; `+OK PUT skip` if unchanged
- `RELOAD` — reload `main.pd` from the active sync target
- `RESET` — `+OK RESET` then reboot

Replies: one line each, `+OK ...` or `-ERR ...`.

## Host tool

```bash
pip install pyserial
cd my_pd_project
python3 scripts/espd_sync.py
```

Optional: `-p /dev/cu.usbmodem1234561` (or `/dev/ttyACM0`, `COM3`, …) if auto-detect fails.

On connect the script **syncs the project tree** (patches, `config.txt`, samples by default). Unchanged files are skipped via CRC. Saving in Pure Data triggers `PUT` + `RELOAD` for changed `.pd` / `config.txt`.

| Flag | Purpose |
|------|---------|
| `--patches-only` | Sync/watch without audio samples |
| `--no-initial-sync` | Watch only |
| `--status` | STATUS and exit |
| `--reset` | Reboot device |
| `--resync-on-reconnect` | Full sync after unplug/reset |
| `--no-reconnect` | Exit when port goes away |
| `--no-color` / `--no-esp-log` | Quieter terminal output |

Do not run `idf.py monitor` on the same port at once.

Quick check:

```bash
python3 scripts/espd_sync.py --status
```

**CRC mismatch / disconnect during PUT:** often USB pacing or re-enumeration — script reconnects when possible; see firmware `espd_dev` task priority and host chunk pacing in script source.

**Logs:** stderr shows `→` / `←` dev traffic; stdout is mostly Pd `print:`.
