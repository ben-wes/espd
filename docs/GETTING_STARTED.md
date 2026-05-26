# Getting started with ESPD

ESPD runs [Pure Data](https://puredata.info/) on ESP32. This guide covers
everything a new user needs: build, flash, configure, and use board I/O from
patches.

**Doc map**

| Doc | What |
|-----|------|
| **[GETTING_STARTED.md](GETTING_STARTED.md)** (this file) | Build, features, configuration |
| **[DEV_SYNC.md](DEV_SYNC.md)** | **Rapid dev** — `espd_sync.py`, OTG CDC, SD sync |
| [BOARD_EXAMPLE_WAVESHARE_S3.md](BOARD_EXAMPLE_WAVESHARE_S3.md) | Waveshare S3 build shortcut + button map |
| [BOARD_EXAMPLE_WAVESHARE_P4_NANO.md](BOARD_EXAMPLE_WAVESHARE_P4_NANO.md) | Waveshare P4-NANO + ESP-Hosted Wi-Fi |
| [ADDING_A_BOARD.md](ADDING_A_BOARD.md) | Add a kit via `boards/*.yaml` |
| [main/espd.h](../main/espd.h) | `config.txt` keys (comments in source) |
| [test-patch/](../test-patch/) | Example `main.pd` + `config.txt` |

## Requirements

- [ESP-IDF **v6.0.1**](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (`install.sh` once in the IDF tree)
- USB serial for flash/monitor
- **Generic I2S:** ESP32 or ESP32-S3 + manual I2S wiring (e.g. INMP441 + MAX98357A)
- **Board kit (e.g. Waveshare):** ESP32-S3 or ESP32-P4-NANO with an esp-bsp package
  (see [boards/](../boards/)); P4 uses onboard C6 for Wi-Fi (ESP-Hosted)

## Clone and prepare

```bash
git clone --recursive <repo-url> espd && cd espd
git submodule update --init --recursive
./scripts/apply-pd-patches.sh
```

Activate IDF in every new shell:

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh
```

## Three configuration layers

ESPD splits configuration by *when* it applies:

| Layer | Where | When | Examples |
|-------|--------|------|----------|
| **Board profile** | `boards/*.yaml` → generated `sdkconfig.defaults` | CMake configure / first build | PSRAM, flash size, codec drivers, CPU affinity |
| **Compile-time** | `idf.py menuconfig` → **ESPD Configuration** | Build time (rebuild to change) | Target board, WiFi compiled in, `espd/ain` support, sample rate default |
| **Runtime** | `config.txt` on SD or SPIFFS | Every boot | `wifi_ssid=`, `ain_pins=`, `audio_sample_rate=` |

**menuconfig** shows compile-time options only. Board YAML **`features.imply`**
turns many menuconfig options on automatically when you pick a kit (e.g. Waveshare
enables ADC, SD card, `espd/ain`, …) — you often only need **Target board** + Save.

## Build paths

### A — Generic I2S (default)

For breadboard I2S audio without a codec driver:

```bash
idf.py set-target esp32          # or esp32s3
idf.py menuconfig                # ESPD Configuration → pins, WiFi, etc.
idf.py build flash monitor
```

Set **Generic board I2S pins** under menuconfig if your wiring differs from the
defaults. SPH0645 microphones are known **not** to work on ESP32.

### B — Waveshare ESP32-S3-AUDIO (or any `boards/*.yaml` kit)

```bash
idf.py set-target esp32s3
idf.py menuconfig                # ESPD Configuration → Target board → Waveshare → Save
idf.py build flash monitor
```

First build downloads the esp-bsp package into `managed_components/` (network
required). See [BOARD_EXAMPLE_WAVESHARE_S3.md](BOARD_EXAMPLE_WAVESHARE_S3.md).

Verify board selection:

```bash
grep ESPD_BOARD sdkconfig
# CONFIG_ESPD_BOARD_WAVESHARE_S3=y
```

If the wrong board is selected, delete `sdkconfig` and run `set-target` again.

### Later builds

```bash
idf.py build flash monitor
```

Switching boards: change **Target board** in menuconfig → Save → `idf.py build`
(or `fullclean` if options look stale).

## Patches and storage

| File | Search order | Purpose |
|------|--------------|---------|
| `main.pd` | `/sdcard` → `/espd_pd` (SPIFFS) | Patch loaded at boot |
| `config.txt` | same | Runtime tuning (WiFi, GPIO I/O, audio DMA) |

Copy examples from [test-patch/](../test-patch/). Without `main.pd`, an embedded
test patch runs only if **Embed fallback test patch** is enabled in menuconfig
(`ESPD_INCLUDEPATCH` — off by default).

## Rapid dev (OTG CDC + microSD)

With **OTG USB + microSD**, edit patches on your computer and hear them on the board
in near real time — no ejecting the card and no copying through the slow internal-flash
USB drive each time. Needs firmware with **TinyUSB OTG**, **SD card**, and **Patch
sync over OTG CDC** (menuconfig or your board profile). Full protocol, ports, and
troubleshooting: **[DEV_SYNC.md](DEV_SYNC.md)**.

1. Flash once; insert a **microSD** card.
2. On the host (any OS — Python 3 + [pyserial](https://pyserial.readthedocs.io/)):

```bash
pip install pyserial
python3 scripts/espd_sync.py -p PORT ./my_pd_project
```

| OS | Typical CDC port (`-p`) |
|----|-------------------------|
| macOS | `/dev/cu.usbmodem*` (OTG; not `cu.debug-console` for flash) |
| Linux | `/dev/ttyACM0` or `/dev/ttyUSB0` |
| Windows | `COM3` (Device Manager) |

Use a glob only if one device matches (e.g. macOS `'/dev/cu.usbmodem*'`).

After the first project sync, saving a small `.pd` in Pure Data and reloading on the
board is effectively **real time** (PUT + `RELOAD` over CDC). The initial connect pass
syncs the tree; unchanged files are skipped via CRC. Large samples are slower — CDC
throughput, not the protocol.

## Feature reference

### Pd audio (always available when audio init succeeds)

| Pd object | Channels | Notes |
|-----------|----------|--------|
| **dac~** | 1–2 (`ESPD_IOCHANS`) | Output; block size 64 samples |
| **adc~** | 1–2 | Input; requires **Enable adc~ capture** in menuconfig (or board `imply`) |

Sample rate: default **48000 Hz** (`ESPD_AUDIO_SAMPLE_RATE` in menuconfig).
Override at boot with `audio_sample_rate=` in `config.txt`. Same value drives
`dac~`/`adc~` and the hardware codec/I²S.

**`adc~`** is audio (mic/line). **`espd/ain/N`** is separate — raw GPIO
analog levels (pots, sensors) via `ain_pins=` in config.txt.

### Pd board I/O receivers

Bind with `[r espd/…]` (or send floats to outputs). Indices are **numeric**, not
named after hardware labels.

| Receiver | Message | Enable |
|----------|---------|--------|
| **espd/din/N** | float `0` / `1` (release / press) | BSP buttons → `espd/din/0..` automatically; extra GPIO via `din_pins=` |
| **espd/led**, **espd/led/N** | list `r g b` (0–255) | Automatic when BSP has LEDs |
| **espd/ain/N** | float (raw level) | menuconfig **espd/ain** + `ain_pins=` in config.txt |
| **espd/aout/N** | float 0..1 (PWM) | menuconfig **espd/aout** + `aout_pins=` |
| **espd/dout/N** | float ≥0.5 → high | menuconfig **espd/dout** + `dout_pins=` |
| **espd/touch/N** | float | menuconfig **espd/touch** + `touch_pins=` |

**Pd networking:** `[netsend]` / `[netreceive]` when WiFi is compiled in and STA
connects (see WiFi below).

**Legacy transport:** TCP/UDP port 4498 for host-driven patch loading (optional;
off by default on BSP boards).

### Buttons (BSP kits)

Physical buttons map to **`espd/din/0`**, **`espd/din/1`**, … — always numeric.

- Order is defined in `boards/*.yaml` → `io.buttons:` (e.g. Waveshare:
  `[VOLUP, PLAY, VOLDOWN]` → din 0 = vol up, 1 = play, 2 = vol down).
- Omit `io.buttons` to use the esp-bsp default enum order.
- Boot log prints the din index map (indices only, not names).
- Your patch assigns meaning: `[r espd/din/1]` for play, etc.

Extra GPIO digital inputs (after BSP buttons): enable **espd/din GPIO** in
menuconfig, set `din_pins=4,5` in `config.txt` → `espd/din/3`, `espd/din/4`, …

### Compile-time options (menuconfig → ESPD Configuration)

Visible in `idf.py menuconfig`. Rebuild after changes.

| Option | Default (Generic) | Purpose |
|--------|-------------------|---------|
| **Target board** | Generic I2S | Generic vs Waveshare / other YAML boards |
| **I/O channel count** | 2 | `dac~` / `adc~` channels |
| **Default audio sample rate** | 48000 | Hz; overridable via config.txt |
| **Enable adc~ capture** | off (Generic) | Microphone / codec input path |
| **Audio backend** | Generic I2S / BSP codec | Auto per board |
| **Enable SD card** | off (Generic) | `/sdcard` mount |
| **USB mass storage + serial** | off (Generic); on (Waveshare S3/P4) | TinyUSB MSC + CDC on OTG |
| **Embed fallback test patch** | off (Generic) | Built-in patch if no main.pd |
| **Enable WiFi** | on | Pd net objects |
| **Join WiFi when main.pd on disk** | off | Generic profile enables; board kits skip STA by default |
| **Legacy espd TCP/UDP transport** | on (Generic) | Port 4498 host transport |
| **Route Pd print / logs to console** | on | UART (Generic) or USB CDC (S3/P4 composite) |
| **Compile espd/ain** | off | GPIO analog inputs (pots/sensors) |
| **Compile espd/touch** | off | Capacitive touch |
| **Compile espd/aout** | off | PWM outputs |
| **Compile espd/din GPIO** | off | Extra GPIO digital in |
| **Compile espd/dout** | off | GPIO digital out |
| **Generic I2S pins** | BCLK 13, WS 33, … | Generic board only |

Board YAML profiles (`features.imply` + `profile:`) preset many of these for kit
users — check what's enabled after selecting your board.

### Runtime options (`config.txt`)

Loaded from SD or SPIFFS at boot. Does **not** appear in menuconfig.

| Key | Purpose |
|-----|---------|
| **wifi_ssid=**, **wifi_password=** | Join AP (STA starts when `wifi_ssid` is non-empty) |
| **log_broadcast_port=** | UDP broadcast of Pd print/errors |
| **audio_sample_rate=** | Hz (8000–192000) |
| **audio_dma_desc_num=**, **audio_dma_frame_num=** | I²S latency tuning |
| **ain_pins=**, **ain_deadband=**, … | GPIO analog inputs |
| **touch_pins=**, … | Touch inputs |
| **aout_pins=**, **aout_pwm_freq_hz=** | PWM outputs |
| **din_pins=**, **din_active_low=**, … | Extra digital inputs |
| **dout_pins=** | Digital outputs |

Full list and semantics: comments in [main/espd.h](../main/espd.h). Example:
[test-patch/config.txt](../test-patch/config.txt).

### Built-in Pd extras

- Vanilla objects including **fft~** / **ifft~** / **rfft~** / **rifft~** (CPU-heavy on ESP32)
- **[pdcontrol]** messages (e.g. `ip` for device address when WiFi is up)
- **[cputime]** — loop timing counter

## Typical Waveshare first boot

1. Flash firmware (build path B above).
2. Put `main.pd` on microSD — copy the card once, or use [DEV_SYNC.md](DEV_SYNC.md) /
   `espd_sync.py` from the host (recommended for ongoing edits).
3. OTG CDC serial shows boot log, Pd `print:`, and dev-sync replies (not `cu.debug-console`).
4. Patch receives button presses on `espd/din/0..2`, LED commands on `espd/led/N`.

Example minimal test (serial print on play button):

```
[r espd/din/1]
|
[print press]
```

## Troubleshooting

| Symptom | Check |
|---------|--------|
| Wrong board / no codec | `grep ESPD_BOARD sdkconfig`; re-run menuconfig |
| No WiFi | `wifi_ssid=` in config.txt; **Enable WiFi** in menuconfig |
| No SD | **Enable SD card**; Waveshare profile enables it via YAML |
| Silent / wrong pitch | `audio_sample_rate=` vs codec; ben-wes esp-bsp fork for Waveshare |
| Partition too large (Generic S3) | Generic profile uses 2 MB default flash; board YAML sets 16 MB |
| `gen_board_plugins.py failed` | Use IDF export (PyYAML in IDF Python env) |

## Adding another board

See [ADDING_A_BOARD.md](ADDING_A_BOARD.md) — one `boards/mykit.yaml` file plus
an esp-bsp package (upstream, fork, or local).
