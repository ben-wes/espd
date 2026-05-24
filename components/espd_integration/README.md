# Integrating a BSP with ESPD

Short reference — full guide: **docs/ADDING_A_BOARD.md**

ESPD does not embed board-specific logic in **main/**. Hardware comes from
**espd_board_*** plugin components (esp-bsp + shim) or in-tree **bsp_*** under
**components/**. No board plugin ships with ESPD; users add their own.

## Naming convention

`CONFIG_ESPD_BOARD_FOO_BAR=y` → folder **components/espd_board_foo_bar/**.
**espd_boards** and root **CMakeLists.txt** discover plugins automatically.

## Optional **bsp_*** contract

Headers in **include/bsp/**:

| Header | Symbols | Notes |
|--------|---------|-------|
| **bsp_io.h** | LED, buttons, **bsp_sdcard_mount(mount_point)** | Board plugin implements; weak stubs otherwise |
| **espd_bsp_audio.h** | **espd_bsp_audio_hw_init()**, **espd_bsp_audio_codec_*** | Board plugin implements hw init; codec I/O in **espd_integration** |

**espd_board.c** calls inits at boot; **espd_io.c** binds **espd/led** when
**bsp_led_count() > 0**.

## Project wiring

1. **menuconfig** → **ESPD Configuration → Target board** → Save.
2. **main/CMakeLists.txt** → **REQUIRES espd_boards** only.

## Audio backends

| menuconfig | File | Backend |
|------------|------|---------|
| Generic I2S | **espd_audio_generic.c** | Manual GPIO I2S |
| BSP codec | **espd_audio_codec.c** | **espd_bsp_audio_*** (**espd_integration** implements codec I/O) |

`esp_codec_dev` is a managed dep of **espd_integration** itself, so board
plugins inherit it transitively via their `path:` dep on espd_integration.

## Adding a new board

See **docs/ADDING_A_BOARD.md** for the full file layout.
