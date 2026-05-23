# ESPD

Pure Data (Pd) on Espressif ESP32 microcontrollers. Core firmware is **board-agnostic**;
hardware is provided by optional **BSP components** selected in **menuconfig**.

Reference board: **Waveshare ESP32-S3-AUDIO** via [esp-bsp](https://github.com/r1ckp0/esp-bsp/tree/waveshare-bsp) (ES8311 DAC, ES7210 mic, buttons, LED strip, SD card). Hardware is fetched by Component Manager (`components/espd_bsp_selector/`); `components/espd_bsp_shim/` adapts it to ESPD.

## Features

- Vanilla Pd objects compiled in, including **fft~** / **ifft~** / **rfft~** / **rifft~** (Ooura, single-precision via `FFTFLT=float`); FFT-heavy patches are CPU-intensive on ESP32
- `netsend`/`netreceive` when WiFi is enabled
- Audio via **dac~** / **adc~** (generic I2S or BSP codec path)
- Board I/O: **espd/din**, **espd/led**, **espd/ain**, **espd/aout**, **espd/touch** (see [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md))
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

On branch **waveshare-bsp-incl** (or any branch with managed esp-bsp), the first
**idf.py build** needs network access to fetch board packages into
**managed_components/**. Commit **dependencies.lock** if you pin versions for CI.

Activate **ESP-IDF v6.0.1** in each new shell. Use **one** method only — do not
mix **`export.sh`**, a project **`(venv)`**, and **`activate_idf_v6.0.1.sh`**
(that causes the “project was configured with … python” error).

Recommended (EIM / Espressif installer layout):

```bash
# Add once to ~/.zshrc or ~/.bashrc:
alias idf='. ~/.espressif/tools/activate_idf_v6.0.1.sh'

deactivate   # if a project (venv) is active
idf          # sources IDF 6.0.1 for this shell
which python # should be .../.espressif/tools/python/v6.0.1/venv/bin/python
```

Then run **`idf.py`** (the activation script wires it to that Python).

Alternative — manual **`export.sh`** (same IDF tree, may use a different Python
env path depending on how IDF was installed):

```bash
deactivate   # if (venv) is active
export IDF_PATH=~/.espressif/v6.0.1/esp-idf
. $IDF_PATH/export.sh
which python
```

If **idf.py** warns that the active Python differs from the last configure
(e.g. after switching between **`export.sh`** and **`activate_idf_v6.0.1.sh`**):

```bash
idf.py fullclean
idf.py build
```

Another option: **`$IDF_PATH/tools/activate.py`** opens a subshell with the
correct environment (see [IDF tools docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/idf-tools.html)).

Build and flash (from the **espd** repo root). **idf.py** subcommands chain; configure before build when **menuconfig** is in the chain:

```bash
# First time on this machine / after deleting sdkconfig:
idf.py set-target esp32s3 menuconfig build flash monitor

# Later builds (board already configured in sdkconfig):
idf.py build flash monitor
```

In menuconfig: **ESPD Configuration** → **Target board → Waveshare ESP32-S3-AUDIO** (default on esp32s3), WiFi, SD, audio, etc. → **Save** → exit; the chained **build** uses the updated **sdkconfig**.

**Switching boards:** [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) (*Switching boards*). Short version: `idf.py menuconfig build` — select board, Save, then build. If you delete **sdkconfig**, include **set-target** (e.g. `idf.py set-target esp32s3 menuconfig build` for Waveshare).

**Generic I2S:** manual GPIO pins under **ESPD Configuration** (INMP441 + MAX98357A, etc.). SPH0645 mic is known **not** to work on ESP32. Legacy presets: `sdkconfig.wroom`, `sdkconfig.lyrat`, `sdkconfig.lyratmini`, `sdkconfig.bn085`.

**Reference board:** Waveshare ESP32-S3-AUDIO (esp-bsp git dep + `espd_bsp_shim`) — select in menuconfig; first build needs network to fetch dependencies.

**LyraT (legacy):** enable ADF in root `CMakeLists.txt`, use `sdkconfig.lyrat*`, export ADF’s IDF instead of standalone IDF.

## Patches and configuration

| File | Location (priority) | Purpose |
|------|---------------------|---------|
| `main.pd` | `/sdcard` → `/espd_pd` (SPIFFS) → USB MSC | Pd patch loaded at boot |
| `config.txt` | same order | WiFi, ain/touch/aout/din/dout pins, **audio_dma_***, etc. |

Without files on SD, SPIFFS at **`/espd_pd`** is used when populated. If no **main.pd** is found, an embedded test patch runs when **PD_INCLUDEPATCH** is enabled in menuconfig (`main/testpatch.c`).

Example patches: [test-patch/](test-patch/). **config.txt** keys: [main/espd.h](main/espd.h).

## Adding another board

[docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) — full BSP guide. Short reference: [components/espd_integration/README.md](components/espd_integration/README.md).

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
components/espd_bsp_selector/  Conditional esp-bsp git deps (Component Manager)
components/espd_bsp_shim/      ESPD ↔ esp-bsp adapter (I/O, etc.)
docs/ADDING_A_BOARD.md        Integrating new hardware
pd/                            Pd submodule
```

## Further reading

- [ESP-IDF get-started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
- [Waveshare ESP32-S3-AUDIO wiki](https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board)
