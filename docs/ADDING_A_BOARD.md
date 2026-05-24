# Adding a board to ESPD

ESPD core firmware is board-neutral. The repo ships **no** board plugin out of
the box — only **Generic I2S** (manual GPIO pins, no codec driver). To support
a specific kit, drop an **espd_board_*** plugin into **components/**. Discovery
is automatic; no edits in **main/** or root **CMakeLists.txt**.

## Naming convention (auto-discovery)

| Kconfig symbol | Plugin folder | Profile defaults |
|----------------|---------------|------------------|
| `ESPD_BOARD_MYKIT` | `components/espd_board_mykit/` | `…/sdkconfig.defaults` |
| `ESPD_BOARD_GENERIC` | *(built-in)* | `main/boards/generic/sdkconfig.defaults` |

Folder suffix must match the Kconfig name (lower case). Root **CMakeLists.txt**
globs `components/espd_board_*` for Kconfig and profile defaults; **espd_boards**
links enabled plugins. Unselected board plugins are **EXCLUDE_COMPONENTS** until
chosen in menuconfig (so Component Manager does not fetch their git deps on a
Generic build).

A board plugin is metadata-only for standard esp-bsp kits: Kconfig entry,
`idf_component.yml`, `sdkconfig.defaults`, and an empty `CMakeLists.txt`.
Shared audio/I/O glue lives in **espd_integration** and is compiled by
**espd_boards** when a non-Generic board is selected. Optional
**espd_board_io_config.h** overrides button mapping. Custom hardware (Path B)
still uses per-board C sources.

All deps — both the local `espd_integration` and any managed esp-bsp package —
are declared in the plugin's `idf_component.yml` (the local one via `path:`).
The plugin's `CMakeLists.txt` therefore needs no `REQUIRES` list at all;
everything propagates through Component Manager. This sidesteps the IDF v6
quirk where a non-empty managed-`reqs` would otherwise replace the local CMake
`REQUIRES` for components added via `EXTRA_COMPONENT_DIRS`.

## Overview

```
  Pd patch  ──►  main/espd.c, espd_io.c          (board-agnostic)
                      │
                      ▼
               main/espd_board.c                 (probes optional bsp_*)
                      │
         ┌────────────┴────────────────────────────┐
         ▼                                          ▼
  espd_integration                    espd_board_* (metadata per kit)
  (weak stubs + codec glue)           Kconfig + idf_component.yml + profile
         │                                          │
         ▼                                          ▼
  espd_board_* (metadata + shared glue SRCS)  managed esp-bsp (Component Manager)
         └──────────────────────────────────────────┘
```

**main/** never names a specific board. **menuconfig** selects the board;
**main/CMakeLists.txt** always **REQUIRES espd_boards**; **espd_boards**
auto-links enabled **espd_board_*** plugins from menuconfig.

## Plugin file layout

For a kit `MYKIT`, create `components/espd_board_mykit/` with these files:

| File | Role |
|------|------|
| **Kconfig.board** | `choice ESPD_BOARD` entry (sourced via auto-generated `Kconfig.inc`) |
| **idf_component.yml** | `path:` dep on `espd_integration` + git/registry dep on the BSP |
| **sdkconfig.defaults** | ESPD profile (`ESPD_USE_ADC`, SD, PSRAM, stacks, …) |
| **CMakeLists.txt** | Shared-glue boilerplate (see below) — no custom `.c` |
| **espd_board_io_config.h** | *(optional)* button → `espd/din/N` map when BSP enum order differs |

Shared glue (no per-board C):

| File | Role |
|------|------|
| **espd_integration/espd_bsp_esp_bsp_audio.c** | `espd_bsp_audio_hw_init()` for all esp-bsp codec boards |
| **espd_integration/espd_bsp_esp_bsp_io.c** | `bsp_io.h` + `espd_bsp_sdcard_mount()` (LED, buttons, SD) |
| **Board plugin CMakeLists.txt** | Compiles glue when `ESPD_BOARD_ESP_BSP_GLUE` (BSP headers) |

**main/espd_audio_codec.c** handles Pd audio policy (sample rate, volume, gain)
via **espd_bsp_audio_***; `esp_codec_dev` glue is shared in
**espd_integration/espd_bsp_codec_dev.c** (declared by espd_integration itself,
inherited transitively via the plugin's `path:` dep).

Selecting the board in **menuconfig** (or the first **build** after) fetches
its esp-bsp into **managed_components/** (network required). **Generic I2S**
fetches nothing.

Display/camera/LVGL deps from upstream esp-bsp are **dead-stripped** from
**espd.bin**.

**Worked example:** see [BOARD_EXAMPLE_WAVESHARE_S3.md](BOARD_EXAMPLE_WAVESHARE_S3.md)
for a complete plugin (Waveshare ESP32-S3-AUDIO via `ben-wes/esp-bsp@waveshare-bsp`).

## Path A — esp-bsp package (preferred when upstream has your board)

1. Confirm the board exists in [esp-bsp](https://github.com/espressif/esp-bsp)
   (or your fork).

2. **Kconfig.board** — add a `choice ESPD_BOARD` entry (`ESPD_BOARD_MYKIT`).
   Keep it minimal; put ESPD/IDF tuning in **sdkconfig.defaults**, not Kconfig
   `select`s. Use `imply` for ESPD feature flags so they update live in
   menuconfig when the board is selected:

   ```kconfig
   config ESPD_BOARD_MYKIT
       bool "My audio kit"
       depends on IDF_TARGET_ESP32S3
       select ESPD_BOARD_ESP_BSP_GLUE
       imply ESPD_USE_ADC
       imply ESPD_PD_USE_SDCARD
       help
         My audio kit (ES8311 codec, SD card, ...).
   ```

   `select ESPD_BOARD_ESP_BSP_GLUE` enables the shared esp-bsp glue in
   **espd_boards**. Omit for Path B custom boards that supply their own C.

3. **idf_component.yml** — list both deps here: `espd_integration` via `path:`,
   and the esp-bsp package via `git:` (or registry name). Registry deps
   (`led_strip`, `button`, `esp_codec_dev`, …) propagate transitively.
   `espd_integration` declares `espressif/esp_codec_dev` itself for the BSP
   codec backend (managed name `espressif__esp_codec_dev`).

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

4. **CMakeLists.txt** — shared-glue boilerplate (same for every esp-bsp kit):

   ```cmake
   if(CONFIG_ESPD_BOARD_ESP_BSP_GLUE)
       set(_espd_glue "${CMAKE_CURRENT_LIST_DIR}/../espd_integration")
       idf_component_register(
           SRCS
               "${_espd_glue}/espd_bsp_esp_bsp_audio.c"
               "${_espd_glue}/espd_bsp_esp_bsp_io.c"
           INCLUDE_DIRS "."
       )
   else()
       idf_component_register()
   endif()
   ```

   Compiles in the board plugin so BSP headers from Component Manager are
   visible. Add **espd_board_io_config.h** only if button order differs.

5. **sdkconfig.defaults** — ESPD feature flags + IDF options for that kit
   (merged automatically when the folder name matches the Kconfig suffix).

6. No edits to root **CMakeLists.txt** or **espd_boards** — discovery is
   automatic.

**Audio:** shared `espd_bsp_esp_bsp_audio.c` implements `espd_bsp_audio_hw_init()`;
`espd_bsp_audio_codec_*` is in `espd_integration/espd_bsp_codec_dev.c`;
`main/espd_audio_codec.c` is Pd policy only.

**I/O:** shared `espd_bsp_esp_bsp_io.c` implements `bsp_io.h` and
`espd_bsp_sdcard_mount()` (expander SD detect via `BSP_SD_DET` when defined).

## Path B — in-tree hardware (no esp-bsp package)

Same layout as Path A, but **`components/espd_board_mykit/`** talks to your
own drivers instead of a managed esp-bsp package. Still use
**`espd_board_*`** for the ESPD adapter — do **not** put **`espd_bsp_*`**
APIs inside a board-neutral BSP.

| Layer | Folder | Knows ESPD? | Implements |
|-------|--------|-------------|------------|
| Hardware (optional) | **`components/bsp_myboard/`** | No | I2S, GPIO, codec chips — plain ESP-IDF |
| ESPD adapter (required) | **`components/espd_board_mykit/`** | Yes | **`bsp_io.h`**, **`espd_bsp_audio_hw_init()`**, Kconfig, profile defaults |

`bsp_io.h` lives in **`espd_integration`** — it is the ESPD I/O contract.
Your `espd_board_mykit` plugin implements those symbols (and
`espd_bsp_sdcard_mount()` if needed), calling into `bsp_myboard` or ESP-IDF
drivers directly.

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
**sdkconfig.defaults.&lt;target&gt;**.

On a fresh configure, the Generic profile is merged until a board is chosen.
**ESPD** options (ADC, SD card, …) update live in menuconfig when you switch
boards (`imply` in `Kconfig.board`). **IDF** tuning (PSRAM, stacks, …) comes
from the board `sdkconfig.defaults` at the next cmake reconfigure — run
`build` after saving menuconfig, or pre-select the board by appending
`CONFIG_ESPD_BOARD_MYKIT=y` to `sdkconfig.defaults.esp32s3` so those defaults
apply on the first menuconfig.

After a board switch, delete stale **sdkconfig** if options look wrong, then
**set-target** + **menuconfig** + **build**.

Activate IDF v6.0.1 per [README.md](../README.md).

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

**espd_storage_init()** mounts SPIFFS at **/espd_pd** and probes paths. SD
mounts once via **espd_storage_mount_sdcard()** (before **config.txt** is
read), then codec init runs in **initdacs()**. Boot order: SPIFFS probe → SD
mount → config load → audio → buttons/Pd bind.

Without SD, **config.txt** and **main.pd** on SPIFFS are used automatically.
See **main/espd_storage.c** and **main/espd.h**.

## Capacitive touch

SoC touch → **espd/touch/N**. **ESPD_PD_USE_TOUCH0** + **touch_pins=** in
**config.txt**.

See **components/espd_integration/README.md**.
