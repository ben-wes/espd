# Rapid patch dev (CDC sync)

Edit patches on the host, hear them on the board **almost immediately** for small
`.pd` / `config.txt` saves. Uses `espd_sync.py` over OTG CDC instead of slow
drag-and-drop to the internal-flash USB volume for every change.

General build and storage rules: **[README.md](../README.md)**.

Works on **macOS, Linux, and Windows** — Python 3 + [pyserial](https://pyserial.readthedocs.io/).

## Boot order (firmware)

See **[USB_AND_WIFI.md](USB_AND_WIFI.md)** for OTG + Wi‑Fi boot order.
Summary: **early `/storage`** → read **`config.txt`** → **Wi‑Fi PHY + CDC** → (optional
first-boot USB drive mode until host ejects) → MSC handed to the app →
`wifi_start_sta` → **`wifi_wait_sta`** → **Pd**. Dev sync: **`STATUS`**, then
**`PUT`** (host script sends **`RESET`** only when `internal=no`).

### Monitor (OTG CDC kits)

```bash
idf.py -p /dev/cu.usbmodem1234561 monitor
python3 scripts/espd_sync.py -p /dev/cu.usbmodem1234561
```

Use **`1234561` only** for runtime. Flash on `101`, then close the port before reset.

### SoftAP / Wi‑Fi sync (`ESPD_WIFI_AP_SYNC`)

Join the device SoftAP (boot log shows `ESPD-XXXX`), then either:

- **Web console:** [https://192.168.4.1/](https://192.168.4.1/) — monitor, Pd control, live folder watch (accept the self-signed cert warning once).
- **Python:** `python3 scripts/espd_sync.py --host 192.168.4.1 ./my_patch` (TCP port **4499**, full folder sync)

Same line protocol as USB CDC. USB Serial JTAG monitor still works when a cable is attached.

## Active store

Boot and CDC sync use the same rule as firmware storage ([espd_storage.c](../main/espd_storage.c)):

| Mounted | Path | Role |
|---------|------|------|
| SD card | `/sdcard` | Preferred when present |
| Internal flash | `/storage` | When SD is absent and MSC partition is mounted |

The host script does **not** pick the store itself. On connect it sends `STATUS` and syncs to whatever the device reports (`sdcard=yes` → `/sdcard`, else `internal=yes` → `/storage`). If `internal=no` (boot still in progress or stuck in USB drive mode), the script sends **`RESET`**, reconnects, and polls until `internal=yes`.

### Flasher / Web Serial (dev mode UI)

1. Open CDC (`cu.usbmodem*123456*1` on typical OTG BSP kits).
2. `internal=yes`: **PUT** directly.
3. `internal=no`: **`RESET`** (or power-cycle / eject the USB drive on first boot), wait for reconnect, then **PUT**.
4. On USB disconnect, re-request the serial port; do not share the port with `idf.py monitor`.

On **internal-flash-only** boards (no SD), `/storage` is **app-only** after boot —
the host USB data volume is torn down, so there is nothing to edit in Finder
during `espd_sync`. Sync is **CDC PUT** only, not drag-and-drop. The host may
see a drive only during the **first-boot** window (until you eject once).

## Reformat internal flash (`/storage`)

Needed when:

- wear-levelling sector size changed (e.g. 512 B → 4 KiB in `sdkconfig.defaults.esp32s3`);
- the FAT is corrupt and mount fails; or
- **undersized `/storage`**: serial shows a large partition (e.g. `/storage: 14784 KiB at 0x190000`) but stats/PUT report only ~400 KB free (`-ERR no space` on multi‑MB files).

That last case is usually a **stale tiny FAT** left in flash from an earlier boot that registered `/storage` against a wrong flash size (typical after a **web flasher** first boot before SFDP-based sizing). Reflashing the app does **not** erase the tail of flash where `/storage` lives, so a valid but tiny FAT can keep mounting.

**SD card (`/sdcard`) is not affected.** Everything previously on `/storage` is lost.

There is **no `storage` row in `partitions_pd.csv`** — firmware registers a runtime partition for “rest of flash” ([espd_usb_register_dynamic_storage](../main/espd_usb.c)). Tools that erase by **partition name** from the burned table cannot target it.

### Option A — flash and boot

Firmware uses **`format_if_mount_failed`** on `/storage`. That only runs when mount **fails** — a valid (even undersized) FAT mounts without reformatting, so this option **does not** fix the stale tiny-FAT case. Use B or C below for that.

```bash
idf.py flash
# reset the board; watch serial for storage mount / format messages
```

### Option B — erase whole flash, then flash (simplest for tiny-FAT recovery)

```bash
idf.py erase-flash
idf.py flash monitor
```

Also clears NVS and forces a full reflash of the app.

### Option C — erase only the `/storage` region

There is no `idf.py erase-partition` command, and **`parttool erase_partition --partition-name=storage` does not apply** (no `storage` entry in the flashed partition table).

Erase from the end of the factory app through the end of the chip. With the default `partitions_pd.csv` (1536K factory app), `/storage` starts at **`0x190000`**. Size = chip flash size − `0x190000` (e.g. **16 MB** chip → `0xE70000` bytes):

```bash
# 16 MB ESP32-S3 example — adjust size for your chip
python -m esptool --port PORT erase_region 0x190000 0xE70000
```

Then reset so firmware mounts and formats an empty FAT at full size.

### Option D — format from the host (normal MSC mode)

1. Quit `espd_sync.py`.
2. Reset the board; `python3 scripts/espd_sync.py --status` should show
   `internal=yes`.
3. Erase/format the USB mass-storage volume on the host (FAT).

Then sync patches again with `espd_sync.py`.

## Firmware / menuconfig

**ESPD Configuration → Storage & patches → USB device (OTG):**

| Option | Purpose |
|--------|---------|
| **Enable TinyUSB device stack** | OTG USB + CDC serial |
| **USB mass storage** | Internal-flash drive on the cable |
| **Patch sync over OTG CDC** | `espd_sync` PUT / RELOAD / STATUS |

Board YAML profiles enable OTG, MSC, and patch sync per kit (see `boards/*.yaml`).

### Flashing

Hold **BOOT**, tap **RESET**, release **BOOT** when esptool connects, then `idf.py flash`. Auto-reset while the TinyUSB app is running often fails.

On macOS, **do not flash on `cu.debug-console`** — use the OTG CDC port (`cu.usbmodem*`) that answers `STATUS` after a normal reset. See esptool output if unsure.

USB product strings: **Component config → TinyUSB**.

## CDC protocol

Host → device:

- `STATUS` — `+OK STATUS sdcard=yes|no internal=yes|no` (`internal` = `/storage` mounted for the app)
- `PUT <relpath> <nbytes> <crc32hex>` — path may contain spaces; `+OK PUT skip` if unchanged
- `LIST` — recursive file list: `+OK LIST begin`, then one `+FILE <relpath>` per file, then `+OK LIST done <n>`
- `RM <relpath>` — delete one file under the active sync target
- `RELOAD` — reload `main.pd` from the active sync target (`.pd` only; not enough for `config.txt`)
- `MSG <pd-message>` — queue one Pd message (`pd_sendmsg` on the audio thread; `;` appended if omitted)
- `RESET` — `+OK RESET` then reboot (host sends this after uploading `config.txt`)

Replies: one line each, `+OK ...` or `-ERR ...`.

## Host tool

```bash
pip install pyserial
cd my_pd_project
python3 scripts/espd_sync.py
```

Optional: `-p /dev/cu.usbmodem1234561` (or `/dev/ttyACM0`, `COM3`, …) if auto-detect fails.

On connect the script **syncs the project tree** (patches, `config.txt`, samples by default). Unchanged files are skipped via CRC. By default it **mirrors** the folder: files on the device that are not in the local project are removed before PUT (full sync only; incremental watch passes skip mirror). Saving a `.pd` triggers `PUT` + `RELOAD`. Saving **`config.txt`** triggers `PUT` + **`RESET`** (Wi‑Fi, GPIO, audio rate, etc. are read only at boot).

| Flag | Purpose |
|------|---------|
| `--no-mirror` | Do not remove device files missing from the local project |
| `--no-initial-sync` | Watch only |
| `--status` | STATUS and exit |
| `--pd-msg TEXT` | Send one `MSG` to Pd and exit (e.g. `'; pd dsp 1'` or `'print hello'`) |
| `--reset` | Reboot device |
| `--reload` | Reload `main.pd` from active store |
| `--resync-on-reconnect` | Full sync after unplug/reset |
| `--no-reconnect` | Exit when port goes away |
| `--no-color` / `--no-esp-log` | Quieter terminal output |

Do not run `idf.py monitor` on the same port at once.

Quick check:

```bash
python3 scripts/espd_sync.py --status
```

**CRC mismatch / disconnect during PUT:** often USB pacing or re-enumeration — script reconnects when possible; see firmware `espd_dev` task priority and host chunk pacing in script source.

**Throughput:** PUT is **stop-and-wait per 8 KiB window** (`+OK PUT ack` after each chunk) on USB and Wi‑Fi alike. Host pacing in `espd_sync.py` (~800 KB/s) helps SD most; **internal flash** is usually slower anyway (wear levelling, per-file `fsync`). Wi‑Fi link speed rarely limits sync — **device storage writes** do. Do not chase larger windows or TCP tuning unless you measure a real bottleneck on SD.

**Logs:** stderr shows `→` / `←` dev traffic; stdout is mostly Pd `print:`.
