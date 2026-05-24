# ESPD

Pure Data (Pd) on Espressif ESP32 microcontrollers. Core firmware is **board-agnostic**;
hardware is provided by optional **BSP components** selected in **menuconfig**.

Out of the box, only **Generic I2S** (manual GPIO pins, no codec driver) is
shipped. To support a specific kit, drop an **espd_board_*** plugin into
**components/** — see [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md).

## Features

- Vanilla Pd objects compiled in, including **fft~** / **ifft~** / **rfft~** / **rifft~** (Ooura, single-precision via `FFTFLT=float`); FFT-heavy patches are CPU-intensive on ESP32
- `netsend`/`netreceive` when WiFi is enabled
- Audio via **dac~** / **adc~** (generic I2S or BSP codec path)
- Board I/O: **espd/din**, **espd/led**, **espd/ain**, **espd/aout**, **espd/touch** (see [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md))
- WiFi for Pd net objects; optional legacy TCP/UDP patch transport
- **main.pd** and **config.txt** from SD card or internal SPIFFS (SD tried first)

## Setup and build

Install [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) and run **`install.sh`** once in the IDF tree.

Clone and prepare Pd:

```bash
git clone --recursive <repo-url> espd && cd espd
git submodule update --init --recursive
./scripts/apply-pd-patches.sh
```

Activate IDF in each new shell:

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh
```

Selecting a **BSP board** in **menuconfig** (or the first **`idf.py build`** after that) fetches its packages into **managed_components/** via Component Manager (network required). **Generic I2S** does not fetch board packages. Commit **dependencies.lock** if you pin versions for CI.

Build and flash (from the repo root):

```bash
# Default: generic ESP32 (no BSP packages)
idf.py set-target esp32 menuconfig build flash monitor

# Later builds:
idf.py build flash monitor
```

In **menuconfig → ESPD Configuration**: **Target board** defaults to **Generic I2S**. Set WiFi, optional SD/audio GPIO pins, etc. → **Save**, then build.

**BSP boards:** when an **espd_board_*** plugin is present in `components/`, it appears under **Target board** in menuconfig. Pick it → Save → **`idf.py build flash monitor`**. See [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) for the full layout.

**Switching boards:** change board in menuconfig, Save, **`idf.py build`**. After deleting **sdkconfig**, run **`set-target`** again.

**Generic I2S** (INMP441 + MAX98357A, etc.): default board; set GPIO pins under **ESPD Configuration**. SPH0645 mic is known **not** to work on ESP32.

## Patches and configuration

| File | Location (priority) | Purpose |
|------|---------------------|---------|
| `main.pd` | `/sdcard` → `/espd_pd` (SPIFFS) → USB MSC | Pd patch loaded at boot |
| `config.txt` | same order | WiFi, ain/touch/aout/din/dout pins, **audio_dma_***, etc. |

Without files on SD, SPIFFS at **`/espd_pd`** is used when populated. If no **main.pd** is found, an embedded test patch runs when **PD_INCLUDEPATCH** is enabled in menuconfig (`main/testpatch.c`).

Example patches: [test-patch/](test-patch/). **config.txt** keys: [main/espd.h](main/espd.h).

## Adding a board

[docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) — full plugin layout and discovery rules. Short reference: [components/espd_integration/README.md](components/espd_integration/README.md).

**Worked example:** [docs/BOARD_EXAMPLE_WAVESHARE_S3.md](docs/BOARD_EXAMPLE_WAVESHARE_S3.md) — Waveshare ESP32-S3-AUDIO via `ben-wes/esp-bsp@waveshare-bsp`.

## WiFi and remote patches

Configure STA via **config.txt** with **wifi_ssid=** (and optional
**wifi_password=**). **wifi_enable** is only for explicit off or forcing STA
when skip-WiFi-on-local-main-pd is enabled. Without **config.txt**, Kconfig
defaults apply.

Optional legacy host transport: board connects on port 4498; load patches with `pd begin-new …` / `pd end-new`. Examples in [test-patch/](test-patch/).

## Project layout

```
main/                          Core ESPD (board-neutral)
components/espd_integration/   bsp_* contract + weak stubs + codec glue
components/espd_boards/        Registry (links enabled espd_board_* plugins)
components/espd_board_waveshare_s3/  Example metadata-only plugin (ben-wes/esp-bsp)
docs/ADDING_A_BOARD.md         Integrating new hardware
pd/                            Pd submodule
```

## Further reading

- [ESP-IDF get-started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
- [esp-bsp board packages](https://github.com/espressif/esp-bsp)
