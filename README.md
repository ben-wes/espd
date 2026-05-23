# ESPD

Pure Data (Pd) on Espressif ESP32 microcontrollers. Core firmware is **board-agnostic**;
hardware is provided by optional **BSP components** selected in **menuconfig**.

Reference board in this repo: [Waveshare ESP32-S3-AUDIO](components/bsp_waveshare_s3/README.txt) (ES8311 DAC, ES7210 mic, buttons, LED strip, SD card).

## Features

- Vanilla Pd objects compiled in, including **fft~** / **ifft~** / **rfft~** / **rifft~** (Ooura, single-precision via `FFTFLT=float`); FFT-heavy patches are CPU-intensive on ESP32
- `netsend`/`netreceive` when WiFi is enabled
- Audio via **dac~** / **adc~** (generic I2S or BSP codec path)
- Board I/O: **espd/din**, **espd/led**, **espd/ain**, **espd/aout**, **espd/touch** (see [docs/ADDING_A_BOARD.txt](docs/ADDING_A_BOARD.txt))
- WiFi for Pd net objects; optional legacy TCP/UDP patch transport
- **main.pd** and **config.txt** from SD card or internal SPIFFS (SD tried first)

## Setup and build

Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) **v6.0.1** (the version this project is built and tested against; other v5+/v6 releases may work). One-time per machine: run **`install.sh`** / **`install.bat`** in the IDF tree.

Clone and prepare Pd:

```bash
git clone --recursive <repo-url> espd && cd espd
git submodule update --init --recursive   # after branch switches too
./scripts/apply-pd-patches.sh
```

Activate the IDF environment in each new shell (pick one — see [IDF tools docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/idf-tools.html)):

```bash
# Recommended (IDF 5.2+): opens a subshell with idf.py on PATH
$IDF_PATH/tools/activate.py

# Or source export in the current shell (same effect for idf.py)
. $IDF_PATH/export.sh
```

Build and flash (from the **espd** repo root). **idf.py** subcommands chain; configure before build when **menuconfig** is in the chain:

```bash
idf.py set-target esp32s3 menuconfig build flash monitor
```

In menuconfig: **ESPD Configuration** → pick **Target board**, WiFi, SD, audio, etc. → **Save** → exit; the chained **build** uses the updated **sdkconfig**.

**Switching boards:** [docs/ADDING_A_BOARD.txt](docs/ADDING_A_BOARD.txt) (*Switching boards*). Short version: `idf.py menuconfig build` — select board, Save, then build.

**Generic I2S:** manual GPIO pins under **ESPD Configuration** (INMP441 + MAX98357A, etc.). SPH0645 mic is known **not** to work on ESP32. Legacy presets: `sdkconfig.wroom`, `sdkconfig.lyrat`, `sdkconfig.lyratmini`, `sdkconfig.bn085`.

**Reference BSP:** [Waveshare ESP32-S3-AUDIO](components/bsp_waveshare_s3/README.txt) — board-specific hardware notes only; switching boards uses the same menuconfig flow as any BSP.

**LyraT (legacy):** enable ADF in root `CMakeLists.txt`, use `sdkconfig.lyrat*`, export ADF’s IDF instead of standalone IDF.

## Patches and configuration

| File | Location (priority) | Purpose |
|------|---------------------|---------|
| `main.pd` | `/sdcard` → `/espd_pd` (SPIFFS) → USB MSC | Pd patch loaded at boot |
| `config.txt` | same order | WiFi, ain/touch/aout/din/dout pins, **audio_dma_***, etc. |

Without files on SD, SPIFFS at **`/espd_pd`** is used when populated. If no **main.pd** is found, an embedded test patch runs when **PD_INCLUDEPATCH** is enabled in menuconfig (`main/testpatch.c`).

Example patches: [test-patch/](test-patch/). **config.txt** keys: [main/espd.h](main/espd.h).

## Adding another board

[docs/ADDING_A_BOARD.txt](docs/ADDING_A_BOARD.txt) — full BSP guide. Short reference: [components/espd_integration/README.txt](components/espd_integration/README.txt).

## WiFi and remote patches

Configure STA via **config.txt** with **wifi_ssid=** (and optional
**wifi_password=**). **wifi_enable** is only for explicit off or forcing STA
when skip-WiFi-on-local-main-pd is enabled. Without **config.txt**, Kconfig
defaults apply.

Optional legacy host transport: board connects on port 4498; load patches with `pd begin-new …` / `pd end-new`. Examples in [test-patch/](test-patch/).

## Project layout

```
main/                          Core ESPD (board-neutral)
components/espd_integration/   bsp_* contract + weak stubs
components/bsp_waveshare_s3/   Reference BSP (example)
docs/ADDING_A_BOARD.txt        Integrating new hardware
pd/                            Pd submodule
```

## Further reading

- [ESP-IDF get-started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
- [Waveshare ESP32-S3-AUDIO wiki](https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board)
