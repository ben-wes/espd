# Worked example: Waveshare ESP32-S3-AUDIO

Metadata-only board plugin for the **Waveshare AI Smart Speaker /
ESP32-S3-AUDIO Board** (ES8311 DAC, ES7210 mic, TCA9555 expander, WS2812
LED ring, 3 buttons, microSD).

Uses [`ben-wes/esp-bsp@waveshare-bsp`](https://github.com/ben-wes/esp-bsp/tree/waveshare-bsp)
— a fork with the fixes ESPD relies on (48 kHz stereo default, codec bus init
order, ES8311/ES7210 MCLK config).

**No C shim files.** Audio and I/O glue is shared in
`espd_integration/espd_bsp_esp_bsp_{audio,io}.c`, compiled by **espd_boards**
when any non-Generic board is selected. The plugin folder only declares deps
and profile defaults.

## Plugin layout (5 files, no `.c`)

```
components/espd_board_waveshare_s3/
├── Kconfig.board              # menuconfig entry + imply flags
├── idf_component.yml          # fetches ben-wes/esp-bsp
├── CMakeLists.txt             # idf_component_register() — no sources
├── sdkconfig.defaults         # ESPD + IDF tuning for this kit
└── espd_board_io_config.h     # optional: button → espd/din map
```

Copy from this repo or see the live files under
[`components/espd_board_waveshare_s3/`](../components/espd_board_waveshare_s3/).

### `CMakeLists.txt`

Identical boilerplate for every standard esp-bsp board (Phase 2 YAML will codegen this):

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

Glue compiles in the board plugin (not espd_boards) so Component Manager BSP
headers are on the include path.

### `espd_board_io_config.h` (optional)

Only needed when BSP button enum order ≠ the `espd/din/0..N` order you want.
Omit entirely for boards where enum order is fine.

```c
#pragma once
#include "bsp/esp-bsp.h"

#define ESPD_BSP_BUTTON_COUNT 3
#define ESPD_BSP_BUTTON_MAP { \
    BSP_BUTTON_VOLUP, \
    BSP_BUTTON_PLAY, \
    BSP_BUTTON_VOLDOWN, \
}
```

SD-card expander setup (`BSP_SD_DET`) is handled automatically by the shared
glue when the BSP header defines it.

## Build

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3
echo CONFIG_ESPD_BOARD_WAVESHARE_S3=y >> sdkconfig.defaults.esp32s3
idf.py menuconfig          # save — board appears under Target board
idf.py build flash monitor
```

First build downloads `ben-wes/esp-bsp` into `managed_components/` (network
required).

## Verifying the BSP fork

```bash
grep -n 'I2S_STD_PHILIP[^S]'                     managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'bsp_audio_codec_bus_init'               managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'sample_rate.*22050\|sample_rate.*48000' managed_components/waveshare_esp32_s3_audio/src/*.c
```

## Sample rate

`CONFIG_ESPD_AUDIO_SAMPLE_RATE` (default 48000) and optional
`audio_sample_rate=` in **config.txt** drive both the codec and Pd's
`sys_getsr()`. The ben-wes fork honours the requested rate.

## Adding another esp-bsp kit

Same five-file pattern — swap `idf_component.yml` git/path, tune
`sdkconfig.defaults`, add `espd_board_io_config.h` only if button order
differs. See [ADDING_A_BOARD.md](ADDING_A_BOARD.md).

Future: a single YAML file will codegen these metadata files (Phase 2).
