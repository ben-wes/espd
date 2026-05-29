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

## Reformat internal flash (`/storage`)

Needed after changing wear-levelling sector size (e.g. 512 B → 4 KiB in
`sdkconfig.defaults.esp32s3`) or a bad/corrupt FAT. **SD card (`/sdcard`) is not
affected.** Everything previously on `/storage` is lost.

### Option A — flash and boot (simplest)

New ESPD firmware enables **`format_if_mount_failed`** on `/storage`. If the old
layout cannot mount, the device formats on first boot (may sit quiet for a few
seconds).

```bash
idf.py flash
# reset the board; watch serial for storage mount / format messages
```

### Option B — erase whole flash, then flash

```bash
idf.py erase-flash
idf.py flash monitor
```

Also clears NVS and forces a full reflash of the app.

### Option C — erase only the `storage` partition

There is no `idf.py erase-partition` command. Use IDF’s **parttool** (replace
`PORT` with your serial device, e.g. `/dev/cu.usbmodem*`):

```bash
. $IDF_PATH/components/partition_table/parttool.py \
  --port PORT erase_partition --partition-name=storage
```

Then reset the board so firmware mounts and formats an empty FAT volume.

### Option D — format from the host (normal MSC mode)

1. Quit `espd_sync.py` (not in `msc_sync`).
2. Reset the board; `python3 scripts/espd_sync.py --status` should show
   `mode=normal`.
3. Erase/format the USB mass-storage volume in Finder or Disk Utility (FAT).

Then sync patches again with `espd_sync.py`.

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
- `RELOAD` — reload `main.pd` from the active sync target (`.pd` only; not enough for `config.txt`)
- `RESET` — `+OK RESET` then reboot (host sends this after uploading `config.txt`)

Replies: one line each, `+OK ...` or `-ERR ...`.

## Host tool

```bash
pip install pyserial
cd my_pd_project
python3 scripts/espd_sync.py
```

Optional: `-p /dev/cu.usbmodem1234561` (or `/dev/ttyACM0`, `COM3`, …) if auto-detect fails.

On connect the script **syncs the project tree** (patches, `config.txt`, samples by default). Unchanged files are skipped via CRC. Saving a `.pd` triggers `PUT` + `RELOAD`. Saving **`config.txt`** triggers `PUT` + **`RESET`** (Wi‑Fi, GPIO, audio rate, etc. are read only at boot).

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

**Throughput:** host pacing in `espd_sync.py` (~800 KB/s) helps SD most. **ESP32-S3** builds use **4 KiB wear-levelling** and aligned MSC/PUT buffers (`sdkconfig.defaults.esp32s3` + `espd_dev.c`); internal flash (`msc_sync`) is still slower than SD because of wear levelling + per-file `fsync`.

**Logs:** stderr shows `→` / `←` dev traffic; stdout is mostly Pd `print:`.
