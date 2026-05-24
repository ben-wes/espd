# Adding a board to ESPD

Integrator guide. End-user setup and full feature list:
**[GETTING_STARTED.md](GETTING_STARTED.md)**.

ESPD core firmware is board-neutral. Out of the box only **Generic I2S** is
shipped (manual GPIO pins, no codec driver). Every other kit is an **esp-bsp**
package plus a one-file YAML definition.

## Quick start

1. Confirm your kit has (or add) an [esp-bsp](https://github.com/espressif/esp-bsp)
   package — upstream, a fork, or a local tree with the usual `bsp/esp-bsp.h`
   API (`bsp_audio_init`, `bsp_iot_button_create`, …).

2. Add **`boards/mykit.yaml`** (see schema below).

3. Configure and build — CMake generates **`components/espd_board_mykit/`**
   automatically on every `idf.py` configure:

```bash
idf.py set-target esp32s3
echo CONFIG_ESPD_BOARD_MYKIT=y >> sdkconfig.defaults.esp32s3   # optional pre-select
idf.py menuconfig build flash monitor
```

**Worked example:** [boards/waveshare_s3.yaml](../boards/waveshare_s3.yaml) →
[BOARD_EXAMPLE_WAVESHARE_S3.md](BOARD_EXAMPLE_WAVESHARE_S3.md).

## Naming convention

| YAML `id:` | Kconfig symbol | Generated folder |
|------------|----------------|------------------|
| `mykit` | `ESPD_BOARD_MYKIT` | `components/espd_board_mykit/` |
| `waveshare_s3` | `ESPD_BOARD_WAVESHARE_S3` | `components/espd_board_waveshare_s3/` |
| *(built-in)* | `ESPD_BOARD_GENERIC` | `main/boards/generic/` |

`id` must be lowercase `[a-z][a-z0-9_]*`. Root **CMakeLists.txt** runs
**scripts/gen_board_plugins.py** before Kconfig and component discovery.

Generated plugin files (**do not edit by hand**):

| File | Role |
|------|------|
| **Kconfig.board** | menuconfig entry + `imply` feature flags |
| **idf_component.yml** | `espd_integration` + esp-bsp git/registry dep |
| **CMakeLists.txt** | compiles shared esp-bsp glue from `espd_integration` |
| **sdkconfig.defaults** | ESPD + IDF profile for this kit |
| **espd_board_io_config.h** | *(optional)* button → `espd/din/N` map |

Shared glue (one copy for all esp-bsp boards):

| File | Role |
|------|------|
| **espd_integration/espd_bsp_esp_bsp_audio.c** | `espd_bsp_audio_hw_init()` |
| **espd_integration/espd_bsp_esp_bsp_io.c** | LEDs, buttons, SD expander detect |
| **espd_integration/espd_bsp_codec_dev.c** | `esp_codec_dev` I/O |

Unselected board plugins are **EXCLUDE_COMPONENTS** until chosen in menuconfig
(Component Manager does not fetch their git deps on a Generic build).

## Architecture

```
  boards/mykit.yaml
        │  (gen_board_plugins.py on cmake configure)
        ▼
  components/espd_board_mykit/     metadata + shared glue SRCS
        │  idf_component.yml
        ▼
  managed esp-bsp package            bsp/esp-bsp.h drivers
        ▲
  espd_integration                 weak stubs + codec glue
        ▲
  main/espd_board.c, espd_io.c     board-agnostic Pd firmware
```

**main/** never names a board. **menuconfig → Target board** selects the kit;
**main** always **REQUIRES espd_boards**, which links the enabled plugin.

## YAML schema

```yaml
id: mykit                          # required → ESPD_BOARD_MYKIT
name: My Audio Kit                 # menuconfig label
target: esp32s3                    # IDF_TARGET_* dependency
help: |                            # optional Kconfig help
  ES8311 codec, SD card, WS2812 ring.

bsp:                               # required — esp-bsp Component Manager dep
  component: my_board_audio        # managed component name
  git: https://github.com/you/esp-bsp.git
  path: bsp/my_board_audio
  version: my-branch               # branch, tag, or commit
  # — or registry instead of git: —
  # version: "^1.0.0"

features:                          # optional — live menuconfig hints
  imply:
    - ESPD_USE_ADC
    - ESPD_PD_USE_SDCARD

io:                                # optional — omit if BSP button order is fine
  buttons: [VOLUP, PLAY, VOLDOWN]  # → BSP_BUTTON_* for espd/din/0..N

profile:                           # sdkconfig.defaults sections
  ESPD features:
    ESPD_USE_ADC: y
    ESPD_PD_USE_SDCARD: y
  Board hardware:
    ESPTOOLPY_FLASHSIZE_16MB: y
    SPIRAM: y
  Pd runtime tuning:
    ESP_DEFAULT_CPU_FREQ_MHZ_240: y
```

Profile keys may omit the `CONFIG_` prefix. Values are `y`/`n`, numbers, or
quoted strings (e.g. `'"/sdcard"'`, `"0x1"`).

SD-card expander detect (`BSP_SD_DET`) is handled automatically by the shared
I/O glue when the esp-bsp header defines it.

## Switching boards

```bash
idf.py menuconfig build flash monitor
```

**ESPD Configuration → Target board** → pick board → **Save** → **build**.

Clean slate / different SoC:

```bash
idf.py set-target esp32s3 fullclean menuconfig build flash monitor
```

Profile defaults merge from **main/boards/generic/** (Generic I2S) or the
generated **components/espd_board_*/sdkconfig.defaults**. Pre-select a board
by appending `CONFIG_ESPD_BOARD_MYKIT=y` to **sdkconfig.defaults.&lt;target&gt;**
before the first menuconfig.

Activate IDF v6.0.1 per [README.md](../README.md).

## Pd I/O surface (core — not board-specific)

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
read), then codec init runs in **initdacs()**.

See **main/espd_storage.c** and **main/espd.h**.

See **components/espd_integration/README.md** for the **bsp_io.h** contract.
