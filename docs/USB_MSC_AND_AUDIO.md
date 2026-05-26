# USB MSC, audio, and what not to repeat

Baseline firmware: commit **`b3e6248`** (`composite usb works`). That init order is
**storage → MSC registration → TinyUSB → CDC → console** — same as Espressif’s
[composite MSC + serial example](https://github.com/espressif/esp-idf/tree/v6.0.1/examples/peripherals/usb/device/tusb_composite_msc_serialdevice).

## Symptoms we saw

| Symptom | Likely cause | Reference |
|--------|----------------|-----------|
| `[cputime]` >> 100% during Finder copy | SPI flash busy: MSC wear-levelling + **XIP from same flash** stalls the CPU | [XIP from PSRAM](https://github.com/espressif/esp-idf/tree/v6.0.1/examples/system/xip_from_psram), [flash/PSRAM guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/flash_psram_config.html) |
| Audio glitches / Pd “frozen” during copy | Same flash bus contention (not “CPU too slow” alone) | Enable `CONFIG_SPIRAM_XIP_FROM_PSRAM` (see below) |
| Copy ~10 s for 1 KB | Often **macOS remount/sync**, not raw USB throughput | Eject volume; avoid reload while host has MSC mounted |
| Brief logs then `Device not configured` | macOS **MSC auto-mount** resets CDC briefly; monitor reconnects | Keep **b3e6248** console init; optional re-attach after MSC events (not in Phase A) |
| `waiting for download` after flash | `idf.py monitor` **RTS reset** on USB-JTAG port | `idf.py monitor --no-reset`, then board **RESET**; logs on `cu.usbmodem*` |
| Build fails on `679870f` | `espd_dev.c` referenced in CMake, **never committed** | Do not cherry-pick that commit whole-cloth |

## Official performance guidance (Espressif)

### 1. Audio during internal-flash MSC — XIP from PSRAM

When `CONFIG_SPIRAM_XIP_FROM_PSRAM` is enabled, `.text` and `.rodata` run from
PSRAM so flash erase/program during MSC does **not** disable concurrent XIP the
same way. Espressif documents the SET1/SET2 conflict in the
[xip_from_psram example README](https://github.com/espressif/esp-idf/tree/v6.0.1/examples/system/xip_from_psram).

**Requirements:** Octal PSRAM (Waveshare S3/P4 profiles already use it), enough
PSRAM for the mapped segments. **Cost:** slightly slower ISR code if handlers
live in flash-mapped PSRAM; keep hot paths in IRAM (`FREERTOS_IN_IRAM`, Pd tick
already `IRAM_ATTR`).

### 2. MSC transfer speed — FIFO size

Espressif states internal SPI flash MSC is **demo-grade**; use SD or external
flash for production throughput:

- [ESP-USB device stack — MSC performance](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html)
- [esp_tinyusb component — MSC performance tables](https://components.espressif.com/components/espressif/esp_tinyusb)

**ESP32-S3 (internal flash, their table):**

| `CONFIG_TINYUSB_MSC_BUFSIZE` | Read | Write |
|-----------------------------|------|-------|
| 512 B (our default at b3e6248) | ~0.57 MB/s | ~0.24 MB/s |
| 8192 B (Espressif default) | ~0.93 MB/s | ~0.93 MB/s |

`CONFIG_TINYUSB_MSC_BUFSIZE` must be **≥ `CONFIG_WL_SECTOR_SIZE`** ([tinyusb_msc.c](https://github.com/espressif/esp-usb/blob/master/device/esp_tinyusb/tinyusb_msc.c)).

Raise TinyUSB device task priority so MSC callbacks drain while the host is
writing (we use prio **4** on CPU0; baseline **2**).

### 3. Wear levelling sector size (optional, destructive)

[IDF wear levelling](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/wear-levelling.html):
**4096 B sectors = best WL performance**; 512 B is supported but slower.

Switching `WL_SECTOR_SIZE` requires **reformatting** the `storage` partition
(existing USB contents lost). Only try after backing up patches.

## What we tried and should **not** repeat without a full design

| Approach | Why it failed or hurt |
|----------|------------------------|
| **Pause Pd while host owns MSC** | macOS keeps volume mounted → patch appears dead |
| **CDC-first USB init + delayed `tinyusb_console_init`** | Broke “everything works” for this board; logs/MSC fragile |
| **`espd_dev` reload / sentinel / sync scripts** | Half-committed (`679870f`); 10 s “sync” = host remount |
| **Probe order SPIFFS before `/storage`** | Wrong `main.pd` when MSC mounted at boot |
| **Expect MSC ≈ SD card speed** | Architectural limit: program + storage share one flash |

## Recommended rollout (incremental)

Test after **each** step; stay on **b3e6248 USB init** in `espd.c`.

1. **Phase A:** board YAML + optional `ESPD_USB_DEVICE_TASK_PRIO=4` — see doc tables.
   Apply one knob at a time after `fullclean build flash`.
2. **ESP_LOG** and Pd `print` share OTG CDC (`stdout` after `tinyusb_console_init`).
   Do not add `esp_log_set_vprintf` unless you mean to move logs off CDC.
3. **Monitor / sync:** `idf.py -p /dev/cu.usbmodem… monitor --no-reset` or `espd_sync.py`.
2. **Phase B (optional):** `WL_SECTOR_SIZE_4096` + reformat `storage` — more copy
   speed, wipes drive.
3. **Fast patch dev (different architecture):** use **microSD** (`/sdcard`) or
   **CDC/WiFi file push** while device keeps `/storage`; do not mount MSC during
   active development.

## Monitor / flash (unchanged from b3e6248)

```bash
idf.py -p /dev/cu.usbmodem* monitor
```

Press **RESET** after plug-in. App `ESP_LOG` and Pd output are on OTG `cu.usbmodem…`;
`cu.debug-console` is for flash.
