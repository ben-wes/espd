# Adding a board to ESPD

ESPD core firmware is board-neutral. Each hardware target is an **espd_board_***
plugin (esp-bsp + shim, or in-tree drivers + shim). Upstream **esp-bsp** and any
optional in-tree **bsp_*** hardware layer stay ESPD-agnostic.

Example plugin: **components/espd_board_waveshare_s3/** — esp-bsp package
`waveshare_esp32_s3_audio` from [ben-wes/esp-bsp @ waveshare-bsp](https://github.com/ben-wes/esp-bsp/tree/waveshare-bsp) (PR pending upstream).
The repo defaults to **Generic I2S**; Waveshare shows the full plugin layout.

## Naming convention (auto-discovery)

| Kconfig symbol | Plugin folder | Profile defaults |
|----------------|---------------|------------------|
| `ESPD_BOARD_WAVESHARE_S3` | `components/espd_board_waveshare_s3/` | `…/sdkconfig.defaults` |
| `ESPD_BOARD_GENERIC` | *(none)* | `main/boards/generic/sdkconfig.defaults` |

Folder suffix must match the Kconfig name (lower case). Root **CMakeLists.txt** globs
`espd_board_*` for Kconfig and profile defaults; **espd_boards** links enabled plugins.
Unselected board plugins are **EXCLUDE_COMPONENTS** until chosen in menuconfig (so
Component Manager does not fetch their git deps on a Generic build).

A board plugin is fully self-contained: Kconfig entry, `idf_component.yml`,
`sdkconfig.defaults`, shim sources, and `CMakeLists.txt` all live in
`components/espd_board_<name>/`. **main/** never names a board.

All deps — both the local `espd_integration` and the managed esp-bsp
package — are declared in the plugin's `idf_component.yml` (the local one
via `path:`). The plugin's `CMakeLists.txt` therefore needs no `REQUIRES`
list at all; everything propagates through Component Manager. This sidesteps
the IDF v6 quirk where a non-empty managed-`reqs` would otherwise replace
the local CMake `REQUIRES` for components added via `EXTRA_COMPONENT_DIRS`.

## Overview

```
  Pd patch  ──►  main/espd.c, espd_io.c          (board-agnostic)
                      │
                      ▼
               main/espd_board.c                 (probes optional bsp_*)
                      │
         ┌────────────┴────────────────────────────┐
         ▼                                          ▼
  espd_integration                    espd_board_* (one folder per kit)
  (weak bsp_* stubs)                  Kconfig + idf_component.yml + shim
         │                                          │
         │                                          ▼
         │                               managed esp-bsp (Component Manager)
         └──────────────────────────────────────────┘
```

**main/** never names a specific board. **menuconfig** selects the board;
**main/CMakeLists.txt** always **REQUIRES espd_boards**; **espd_boards** auto-links
enabled **espd_board_*** plugins from menuconfig.

## Waveshare plugin (reference)

| File in **espd_board_waveshare_s3/** | Role |
|-----------------------------------|------|
| **Kconfig.board** | **choice ESPD_BOARD** entry (sourced via generated **Kconfig.inc**) |
| **idf_component.yml** | `path:` dep on `espd_integration` + git dep on `waveshare_esp32_s3_audio` |
| **sdkconfig.defaults** | ESPD profile (**ESPD_USE_ADC**, SD, PSRAM, stacks, …) |
| **waveshare_io.c** | **bsp_io.h**, **espd_bsp_sdcard_mount()** |
| **waveshare_audio.c** | **espd_bsp_audio_hw_init()** |
| **CMakeLists.txt** | registers sources when `CONFIG_ESPD_BOARD_WAVESHARE_S3` (no `REQUIRES`) |

**main/espd_audio_codec.c** handles Pd audio policy (sample rate, volume, gain)
via **espd_bsp_audio_***; **esp_codec_dev** glue is **espd_integration/espd_bsp_codec_dev.c**
(fetched transitively from the esp-bsp board package).

Selecting the board in **menuconfig** (or the first **build** after) fetches esp-bsp
into **managed_components/** (network required). **Generic I2S** fetches nothing.

Display/camera/LVGL deps from upstream esp-bsp are **dead-stripped** from **espd.bin**.

## Adding another board (two paths)

### Path A — esp-bsp package (preferred when upstream has your board)

Copy **components/espd_board_waveshare_s3/** as **components/espd_board_mykit/** and adapt
(name the folder to match your Kconfig symbol suffix, e.g. `ESPD_BOARD_MYKIT` →
`espd_board_mykit/`):

1. Ensure the board exists in [esp-bsp](https://github.com/espressif/esp-bsp) (or your fork).

2. **Kconfig.board** — add a **choice ESPD_BOARD** entry (`ESPD_BOARD_MYKIT`). Keep it
   minimal; put ESPD/IDF tuning in **sdkconfig.defaults**, not Kconfig `select`s.

3. **idf_component.yml** — list both deps here: `espd_integration` via
   `path:` (so it propagates through Component Manager rather than CMake
   `REQUIRES`), and the esp-bsp package via `git:`. Registry deps
   (**led_strip**, **button**, **esp_codec_dev**, …) propagate from esp-bsp
   transitively. **espd_integration** declares **espressif/esp_codec_dev**
   itself for the BSP codec backend (managed name **`espressif__esp_codec_dev`**).

   ```yaml
   dependencies:
     idf: ">=6.0.1,<6.1"
     espd_integration:
       path: ../espd_integration
     my_board_audio:
       git: https://github.com/espressif/esp-bsp.git
       path: bsp/my_board_audio
       version: master
   ```

4. **Shim sources** — implement **bsp_io.h** and **espd_bsp_audio_hw_init()** against
   that board's **bsp/esp-bsp.h** (see **waveshare_io.c** / **waveshare_audio.c**).
   Codec open/read/write is shared in **espd_integration/espd_bsp_codec_dev.c**.

5. **CMakeLists.txt** — register sources guarded by `if(CONFIG_ESPD_BOARD_MYKIT)`.
   No `REQUIRES` line — all deps come from `idf_component.yml`.

6. **sdkconfig.defaults** — ESPD feature flags + IDF options for that kit (merged
   automatically when the folder name matches the Kconfig suffix).

7. No edits to root **CMakeLists.txt** or **espd_boards** — discovery is automatic.

**Audio:** board plugin implements **espd_bsp_audio_hw_init()** only; **espd_bsp_audio_codec_***
is shared in **espd_integration**; **main/espd_audio_codec.c** is Pd policy only.

**SD card:** override **espd_bsp_sdcard_mount()** when esp-bsp needs expander setup.
Use **ESPD_BSP_IO_NO_SDCARD_DECL** when including **bsp_io.h** and **esp-bsp.h**.

### Path B — in-tree hardware (no esp-bsp package)

Same layout as Path A, but **`components/espd_board_mykit/`** talks to your own
drivers instead of a managed esp-bsp package. Still use **`espd_board_*`** for the
ESPD adapter — do **not** put **`espd_bsp_*`** APIs inside a board-neutral BSP.

| Layer | Folder | Knows ESPD? | Implements |
|-------|--------|-------------|------------|
| Hardware (optional) | **`components/bsp_myboard/`** | No | I2S, GPIO, codec chips — plain ESP-IDF |
| ESPD adapter (required) | **`components/espd_board_mykit/`** | Yes | **`bsp_io.h`**, **`espd_bsp_audio_hw_init()`**, Kconfig, profile defaults |

**`bsp_io.h`** lives in **`espd_integration`** — it is the ESPD I/O contract. Your
**`espd_board_mykit`** plugin implements those symbols (and **`espd_bsp_sdcard_mount()`**
if needed), calling into **`bsp_myboard`** or ESP-IDF drivers directly.

Prefer Path A when esp-bsp already has your kit.

## Switching boards

```bash
idf.py menuconfig build flash monitor
```

**ESPD Configuration → Target board** → pick board → **Save**.

Clean slate / different SoC:

```bash
idf.py set-target esp32s3 fullclean menuconfig build flash monitor
```

Profile defaults merge from **main/boards/generic/** (Generic I2S) or
**components/espd_board_*/sdkconfig.defaults** (BSP boards). CMake reads
**sdkconfig** when it exists, otherwise **sdkconfig.defaults** plus
**sdkconfig.defaults.&lt;target&gt;** (see **sdkconfig.defaults.esp32s3.example** to pre-select Waveshare).

On a fresh configure, the Generic profile is merged until a board is chosen.
**ESPD** options (ADC, SD card, …) update live in menuconfig when you switch
boards (**imply** in **Kconfig.board**). **IDF** tuning (PSRAM, stacks, …) comes
from the board **sdkconfig.defaults** at the next cmake reconfigure — run
**build** after saving menuconfig, or pre-select the board in
**sdkconfig.defaults.esp32s3** to see those defaults on the first menuconfig.

After a board switch, delete stale **sdkconfig** if options look wrong, then
**set-target** + **menuconfig** + **build**.

Activate IDF v6.0.1 per [README.md](../README.md).

## Build and verify (Waveshare example)

```bash
./scripts/apply-pd-patches.sh
idf.py set-target esp32s3 menuconfig build flash monitor
```

**Target board → Waveshare ESP32-S3-AUDIO** → **Save**.

Boot log: **espd_board** optional inits; **espd_io** binds **espd/led** when
**bsp_led_count() > 0**; **espd/din/0..2** from K1/K2/K3.

## Pd I/O surface (core — not BSP-specific)

| Pd receiver | Source | Enable |
|-------------|--------|--------|
| **dac~** / **adc~** | Audio backend | **ESPD_USE_ADC** for input |
| **espd/din/N** | **bsp_button_*** + optional **din_pins=** | BSP automatic; **ESPD_PD_USE_DIN0** + **din_pins=** for extra GPIO |
| **espd/led**, **espd/led/N** | **bsp_led_*** | Automatic if BSP has LEDs |
| **espd/ain/N** | ADC1 GPIOs | **ESPD_PD_USE_ANALOG0** + **ain_pins=** |
| **espd/aout/N** | LEDC PWM | **ESPD_PD_USE_AOUT** + **aout_pins=** |
| **espd/dout/N** | GPIO out | **ESPD_PD_USE_DOUT0** + **dout_pins=** |
| **espd/touch/N** | Touch sensor | **ESPD_PD_USE_TOUCH0** + **touch_pins=** |

## Local storage (main.pd, config.txt)

**espd_storage_init()** mounts SPIFFS at **/espd_pd** and probes paths. SD mounts
once via **espd_storage_mount_sdcard()** (before **config.txt** is read), then
codec init runs in **initdacs()**. Boot order: SPIFFS probe → SD mount → config
load → audio → buttons/Pd bind.

Without SD, **config.txt** and **main.pd** on SPIFFS are used automatically.
See **main/espd_storage.c** and **main/espd.h**.

## Capacitive touch

SoC touch → **espd/touch/N**. **ESPD_PD_USE_TOUCH0** + **touch_pins=** in
**config.txt**. Waveshare has no touch pads (buttons → **espd/din/N**).

See **components/espd_integration/README.md**.
