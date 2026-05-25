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

Generated plugin files (**do not edit by hand**; created on `idf.py` configure,
not committed — see `.gitignore`):

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
  components/espd_board_mykit/     thin plugin: espd profile + espd_integration
        │  idf_component.yml → pulls BSP package only
        ▼
  BSP package (esp-bsp)            hardware: deps, drivers, bsp_* API
        ▲
  espd_integration                 thin glue → bsp_* / weak stubs
        ▲
  main/espd_board.c, espd_io.c     board-agnostic Pd firmware
```

**main/** never names a board. **menuconfig → Target board** selects the kit;
**main** always **REQUIRES espd_boards**, which links the enabled plugin.

### Delegate to the BSP package

The **board support package** (esp-bsp layout: `bsp/esp-bsp.h`, `bsp_audio_init`,
`bsp_sdcard_mount`, `idf_component.yml`, Kconfig) should own:

- Component Manager dependencies (`esp_codec_dev`, `espressif/usb`, LCD drivers, …)
- IDF 6 `CMakeLists.txt` `REQUIRES` (`esp_driver_gpio`, `esp_driver_i2s`, …)
- Pinout, codec, SD, display, touch

The **espd board YAML** should only point at that package and add **espd-specific**
policy:

| Belongs in BSP package | Belongs in `boards/*.yaml` |
|----------------------|----------------------------|
| `idf_component.yml` deps & versions | `features.imply` (ESPD_USE_*) |
| `CMakeLists.txt` peripheral requires | `profile` (PSRAM, BSP Kconfig, Pd tuning) |
| `bsp_*` init / mount APIs | `io.buttons` → `espd/din/N` map (optional) |
| IDF / API updates for new IDF releases | Help text, `target:` |

**Worked examples:** [boards/waveshare_s3.yaml](../boards/waveshare_s3.yaml) and
[boards/waveshare_p4_nano.yaml](../boards/waveshare_p4_nano.yaml) pull
[`ben-wes/esp-bsp@waveshare-s3-p4`](https://github.com/ben-wes/esp-bsp/tree/waveshare-s3-p4)
(`bsp/waveshare_esp32_s3_audio`, `bsp/esp32_p4_nano`). Hardware deps and IDF 6
fixes live in that fork, not in espd.

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
    - ESPD_USE_SDCARD

io:                                # optional — omit if BSP button order is fine
  buttons: [VOLUP, PLAY, VOLDOWN]  # → BSP_BUTTON_* for espd/din/0..N

profile:                           # sdkconfig.defaults sections
  ESPD features:
    ESPD_USE_ADC: y
    ESPD_USE_SDCARD: y
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
| **espd/din/N** | **bsp_button_*** + optional **din_pins=** | BSP automatic; **ESPD_USE_DIN** + **din_pins=** for extra GPIO |
| **espd/led**, **espd/led/N** | **bsp_led_*** | Automatic if BSP has LEDs |
| **espd/ain/N** | GPIO analog in (pots, sensors) | **ESPD_USE_AIN** + **ain_pins=** |
| **espd/aout/N** | LEDC PWM | **ESPD_USE_AOUT** + **aout_pins=** |
| **espd/dout/N** | GPIO out | **ESPD_USE_DOUT** + **dout_pins=** |
| **espd/touch/N** | Touch sensor | **ESPD_USE_TOUCH** + **touch_pins=** |

## Local storage (main.pd, config.txt)

**[espd_storage_init()](../main/espd_storage.c)** mounts SPIFFS at **/espd_pd**
and probes paths. SD mounts once via
**[espd_storage_mount_sdcard()](../main/espd_storage.c)** (before **config.txt**
is read), then codec init runs in **[initdacs()](../main/espd.c)**.

See [main/espd_storage.c](../main/espd_storage.c), [main/espd.h](../main/espd.h),
and [components/espd_integration/README.md](../components/espd_integration/README.md)
for the **bsp_io.h** contract.
