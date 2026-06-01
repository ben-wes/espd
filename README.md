# ESPD

Pure Data (Pd) on Espressif ESP32 microcontrollers

Requires [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html).

## Documentation

| Doc | For |
|-----|-----|
| **README.md** (this file) | Build, flash, configuration, features |
| [docs/DEV_SYNC.md](docs/DEV_SYNC.md) | Rapid dev — `espd_sync.py`, CDC protocol, ports |
| [docs/BOARD_EXAMPLE_WAVESHARE_S3.md](docs/BOARD_EXAMPLE_WAVESHARE_S3.md) | Waveshare ESP32-S3-AUDIO build shortcut |
| [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) | Adding a kit (`boards/*.yaml`) |
| [main/espd.h](main/espd.h) | `config.txt` keys (comments in source) |
| [test-patch/](test-patch/) | Example `main.pd` + `config.txt` |

## Requirements

- ESP-IDF **v6.0.1** (`install.sh` once in the IDF tree)
- USB serial for flash/monitor
- **Generic I2S:** ESP32 or ESP32-S3 + manual I2S wiring (e.g. INMP441 + MAX98357A)
- **Board kit:** ESP32-S3 with an esp-bsp package (see [boards/](boards/))

## Clone and prepare

```bash
git clone --recursive <repo-url> espd && cd espd
git submodule update --init --recursive   # pd @ 0.56-3
./scripts/apply-pd-patches.sh
```

Activate IDF in every new shell:

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh
```

## Configuration layers

| Layer | Where | When | Examples |
|-------|--------|------|----------|
| **Board profile** | `boards/*.yaml` → generated `sdkconfig.defaults` | CMake configure | PSRAM, flash size, codec drivers |
| **Compile-time** | `idf.py menuconfig` → **ESPD Configuration** | Rebuild to change | Target board, WiFi, `espd/ain`, sample rate |
| **Runtime** | `config.txt` on the active store | Every boot | `wifi_ssid=`, `ain_pins=`, `audio_sample_rate=` |

Board YAML **`features.imply`** turns on many menuconfig options when you pick a kit (e.g. ADC, SD, CDC sync on OTG BSP boards).

## Build

### Generic I2S (default)

Breadboard I2S without a codec driver:

```bash
idf.py set-target esp32          # or esp32s3
idf.py menuconfig                # ESPD Configuration → pins, WiFi, etc.
idf.py build flash monitor
```

Set **Generic board I2S pins** in menuconfig if wiring differs. SPH0645 microphones are known **not** to work on ESP32.

### Board kit (`boards/*.yaml`)

```bash
idf.py set-target esp32s3        # or the YAML `target:`
idf.py menuconfig                # ESPD Configuration → Target board → your kit → Save
idf.py build flash monitor
```

First build downloads esp-bsp into `managed_components/` (network required). Worked example: [docs/BOARD_EXAMPLE_WAVESHARE_S3.md](docs/BOARD_EXAMPLE_WAVESHARE_S3.md). Prebuilt images: [espd-kits](https://github.com/ben-wes/espd-kits).

**Required:** pick the board in menuconfig (chip defaults stay generic until the profile applies). Verify: `grep CONFIG_ESPD_BOARD_ sdkconfig` matches your kit — if wrong, delete `sdkconfig` and run `set-target` again.

Later builds: `idf.py build flash monitor`. Switching boards: change **Target board** → Save → `idf.py build` (or `fullclean` if options look stale).

## Patches and storage

Firmware picks **one** local patch store by what is **mounted**, not by searching both volumes for files:

| Priority | Mount | When |
|----------|-------|------|
| 1 | `/sdcard` | SD card built in and mounted |
| 2 | `/storage` | USB MSC internal flash partition mounted |

`main.pd` and `config.txt` are loaded only from that active mount. There is no SPIFFS store and no mixing (e.g. `main.pd` on SD with `config.txt` only on flash).

If no store is mounted or `main.pd` is missing, menuconfig **Embed fallback test patch** (`ESPD_INCLUDEPATCH`, off by default) can supply a built-in demo.

Copy examples from [test-patch/](test-patch/). Path resolution is implemented in [main/espd_storage.c](main/espd_storage.c) and documented in [main/espd_storage.h](main/espd_storage.h).

## Rapid dev

With **OTG USB** and firmware **Patch sync over OTG CDC** (enabled by many board YAML profiles), edit patches on the host and reload on the board without re-flashing. Sync target is **SD when mounted, else internal `/storage`** — the device reports which via `STATUS`. See [docs/DEV_SYNC.md](docs/DEV_SYNC.md) and [docs/USB_AND_WIFI.md](docs/USB_AND_WIFI.md) (OTG serial, boot order).

```bash
pip install pyserial
cd my_pd_project
python3 scripts/espd_sync.py
```

Optional: `-p PORT` if auto-detect fails; project path defaults to the current directory.

## Feature reference

### Pd audio

| Object | Channels | Notes |
|--------|----------|--------|
| **dac~** | 1–2 (`ESPD_IOCHANS`) | Output; block size 64 samples |
| **adc~** | 1–2 | Requires **Enable adc~ capture** (or board `imply`) |

Default **48000 Hz** (`ESPD_AUDIO_SAMPLE_RATE`); override with `audio_sample_rate=` in `config.txt`.

**`adc~`** is audio (mic/line). **`espd/ain/N`** is separate — raw GPIO analog via `ain_pins=` in `config.txt`.

### Pd board I/O receivers

Bind with `[r espd/…]`. Indices are **numeric**.

| Receiver | Message | Enable |
|----------|---------|--------|
| **espd/din/N** | float `0` / `1` | BSP buttons → `espd/din/0..`; extra GPIO via `din_pins=` |
| **espd/led**, **espd/led/N** | list `r g b` (0–255) | When BSP has LEDs |
| **espd/ain/N** | float | **espd/ain** + `ain_pins=` |
| **espd/aout/N** | float 0..1 (PWM) | **espd/aout** + `aout_pins=` |
| **espd/dout/N** | float ≥0.5 → high | **espd/dout** + `dout_pins=` |
| **espd/touch/N** | float | **espd/touch** + `touch_pins=` |

**Networking:** `[netsend]` / `[netreceive]` when WiFi is compiled in and STA connects.

**Legacy transport:** TCP/UDP port 4498 (optional; off by default on BSP boards).

### Buttons (BSP kits)

Physical buttons map to **`espd/din/0`**, **`espd/din/1`**, … Order is `io.buttons:` in `boards/*.yaml`. Boot log prints the din index map.

### menuconfig (ESPD Configuration)

| Option | Default (Generic) | Purpose |
|--------|-------------------|---------|
| **Target board** | Generic I2S | Generic vs YAML board kits |
| **I/O channel count** | 2 | `dac~` / `adc~` channels |
| **Default audio sample rate** | 48000 | Overridable via `config.txt` |
| **Enable adc~ capture** | off (Generic) | Mic / codec input |
| **Enable SD card** | off (Generic) | `/sdcard` |
| **USB mass storage + serial** | off (Generic); on (OTG BSP kits) | TinyUSB MSC + CDC |
| **Patch sync over OTG CDC** | off (Generic); on (OTG BSP kits) | `espd_sync.py` |
| **Embed fallback test patch** | off | If no `main.pd` on active store |
| **Enable WiFi** | on | Pd net objects |
| **Compile espd/ain**, **touch**, **aout**, **din GPIO**, **dout** | off | GPIO extras |

Full `config.txt` keys: [main/espd.h](main/espd.h). Example: [test-patch/config.txt](test-patch/config.txt).

## Typical BSP kit first boot

1. Flash (build path above).
2. Put `main.pd` on the active store — copy the SD card once, or use `espd_sync.py` (see [docs/DEV_SYNC.md](docs/DEV_SYNC.md)).
3. Patch receives buttons on `espd/din/N`, LEDs on `espd/led/N` per your board YAML.

Minimal test (play button → serial print):

```
[r espd/din/1]
|
[print press]
```

## Troubleshooting

| Symptom | Check |
|---------|--------|
| Wrong board / no codec | `grep ESPD_BOARD sdkconfig`; re-run menuconfig |
| No WiFi | `wifi_ssid=` in `config.txt`; **Enable WiFi** in menuconfig |
| No SD | **Enable SD card**; many board YAML profiles enable it via `features.imply` |
| `gen_board_plugins.py failed` | Use IDF export (PyYAML in IDF Python env) |
| Dev sync / STATUS fails | [docs/DEV_SYNC.md](docs/DEV_SYNC.md); firmware must have **Patch sync over OTG CDC** |

## Adding another board

See [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md).
