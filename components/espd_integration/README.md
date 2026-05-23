# Integrating a BSP with ESPD

Short reference — full guide: **docs/ADDING_A_BOARD.md**

ESPD does not embed board-specific logic. Core firmware probes optional
**bsp_*** symbols; hardware comes from **esp-bsp** (Component Manager) plus
**espd_bsp_shim**, or from an in-tree BSP under **components/**.

## Waveshare (reference)

| Component | Purpose |
|-----------|---------|
| **espd_bsp_selector** | Kconfig + conditional git dep on `waveshare_esp32_s3_audio` |
| **espd_bsp_shim** | **bsp_io.h** (LED, buttons), SD mount helper |
| **espd_integration** | Weak stubs when nothing else is linked |

Audio: **main/espd_audio_codec.c** includes **bsp/esp-bsp.h** and configures
48 kHz stereo I2S before opening ES8311/ES7210 via **esp_codec_dev**.

## Optional **bsp_*** contract

Headers in **include/bsp/**:

| Header | Symbols | Notes |
|--------|---------|-------|
| **bsp_io.h** | LED, buttons, **bsp_sdcard_mount(mount_point)** | Shim implements for Waveshare; weak stubs otherwise |
| **bsp_audio.h** | void **bsp_audio_init()**, codec inits | Used only for in-tree BSPs; esp-bsp path uses **bsp/esp-bsp.h** |

**espd_board.c** calls inits at boot; **espd_io.c** binds **espd/led** when
**bsp_led_count() > 0**. No **espd_port.c** in BSP packages.

## Project wiring

1. **menuconfig** → **ESPD Configuration → Target board** → Save.
2. **main/CMakeLists.txt** → **REQUIRES** includes **espd_bsp_shim** and
   **espd_bsp_selector**; adds the managed BSP component when the board is selected.
3. First build fetches esp-bsp (network required); **dependencies.lock** is updated.

Switching boards: **docs/ADDING_A_BOARD.md** (*Switching boards*).

## Audio backends

| menuconfig | File | Backend |
|------------|------|---------|
| Generic I2S | **espd_audio_generic.c** | Manual GPIO I2S |
| BSP codec | **espd_audio_codec.c** | esp-bsp + **esp_codec_dev** |

## GPIO / touch / storage

- BSP buttons → **espd/din/0..**; extra GPIO din → **ESPD_PD_USE_DIN0** + **din_pins=**
- **espd/dout**, **espd/aout**, **espd/ain**, **espd/touch** — see **docs/ADDING_A_BOARD.md**
- **main.pd** / **config.txt** — SD (**/sdcard**) then SPIFFS (**/espd_pd**)

## Adding a new board

Prefer esp-bsp + **espd_bsp_selector** + **espd_bsp_shim** (see full doc). In-tree
**components/bsp_myboard/** remains valid for boards not in esp-bsp.
