# Adding a board to ESPD

ESPD core firmware is board-neutral. Hardware comes from **esp-bsp** (Component
Manager git deps) plus a thin **espd_bsp_shim** adapter, or from an optional
in-tree BSP component under **components/**.

Reference board: **Waveshare ESP32-S3-AUDIO** — esp-bsp package
`waveshare_esp32_s3_audio` from [r1ckp0/esp-bsp @ waveshare-bsp](https://github.com/r1ckp0/esp-bsp/tree/waveshare-bsp).

## Overview

```
  Pd patch  ──►  main/espd.c, espd_io.c          (board-agnostic)
                      │
                      ▼
               main/espd_board.c                 (probes optional bsp_*)
                      │
         ┌────────────┴────────────────────────────┐
         ▼                                          ▼
  espd_integration                         espd_bsp_shim (per board)
  (weak bsp_* stubs)                       adapts esp-bsp → bsp_io.h
         │                                          │
         │                                          ▼
         │                               espd_bsp_selector
         │                               (conditional git dep on esp-bsp)
         └──────────────────────────────────────────┘
```

**main/** never names a specific board. **menuconfig** selects the board;
**main/CMakeLists.txt** pulls in **espd_bsp_shim** and **espd_bsp_selector**;
the selector fetches the matching esp-bsp package when `CONFIG_ESPD_BOARD_*` is set.

## Waveshare (managed esp-bsp) — how it is wired today

| Piece | Role |
|-------|------|
| **components/espd_bsp_selector/** | Kconfig board choice; **idf_component.yml** git-dep on `waveshare_esp32_s3_audio` when `ESPD_BOARD_WAVESHARE_S3` |
| **espd_bsp_shim** | **bsp_io.h**, **espd_bsp_sdcard_mount()**, **espd_bsp_audio_hw_init()** on esp-bsp |
| **main/espd_audio_codec.c** | Pd audio policy (48 kHz, **esp_codec_dev_open**, read/write) |
| **components/espd_integration/** | Weak **bsp_*** stubs when no hardware is linked |

First build needs **network access** to fetch esp-bsp into **managed_components/**;
**dependencies.lock** is updated automatically.

Display/camera/LVGL deps are declared by upstream esp-bsp but **dead-stripped**
from **espd.bin** — ESPD does not init the LCD or camera.

## Adding another board (two paths)

### Path A — esp-bsp package (preferred when upstream has your board)

1. Ensure the board exists in [esp-bsp](https://github.com/espressif/esp-bsp)
   (or your fork, e.g. **r1ckp0/esp-bsp** until merged upstream).

2. In **components/espd_bsp_selector/idf_component.yml**, add a conditional git
   dependency (same pattern as Waveshare):

   ```yaml
   my_board_audio:
     git: https://github.com/r1ckp0/esp-bsp.git
     path: bsp/my_board_audio
     version: my-branch-or-tag
     public: true
     matches:
       - if: $CONFIG{ESPD_BOARD_MYBOARD} == True
   ```

3. In **components/espd_bsp_selector/Kconfig**, add a **choice ESPD_BOARD** entry,
   **select** rules for the ESPD profile (PSRAM, **ESPD_USE_ADC**, …), and stack
   defaults if needed.

4. Add **components/espd_bsp_shim/** sources (or a new file) that implement
   **bsp_io.h** against that board's **bsp/esp-bsp.h**. Guard with
   `CONFIG_ESPD_BOARD_MYBOARD`.

5. Put IDF tuning in **components/espd_bsp_selector/sdkconfig.defaults** and
   extend root **CMakeLists.txt** board-profile merge if the board name is not
   Waveshare (see the `CONFIG_ESPD_BOARD_WAVESHARE_S3` branch).

6. Wire **main/CMakeLists.txt**: **REQUIRES** **espd_bsp_shim** and
   **espd_bsp_selector** (managed BSP deps are pulled by the shim/selector).

**Audio:** implement **espd_bsp_audio_hw_init()** in **espd_bsp_shim** (I2S GPIO +
**bsp_audio_init**, codec device handles). **main/espd_audio_codec.c** opens codecs
for Pd (sample rate, volume, gain) — do not move **esp_codec_dev_open** into the shim.

**SD card:** esp-bsp may expose **bsp_sdcard_mount(void)** (Kconfig mount point)
while ESPD uses **espd_bsp_sdcard_mount(mount_point)** — a weak default forwards to
**bsp_sdcard_mount(const char *)** from **bsp_io.h**. Managed boards override
**espd_bsp_sdcard_mount** in **espd_bsp_shim** (expander setup, idempotent mount).
Use **ESPD_BSP_IO_NO_SDCARD_DECL** when including both **bsp_io.h** and **esp-bsp.h**.

### Path B — in-tree BSP component (generic / custom hardware)

Use when the board is not in esp-bsp. Layout:

```
components/bsp_myboard/
  CMakeLists.txt
  Kconfig
  sdkconfig.defaults
  include/bsp/
    config.h
    bsp_io.h hooks via myboard.h   # optional split
  myboard.c                        # audio, I2C, …
  bsp_led.c, bsp_button.c, …       # optional
```

Implement the optional API in **components/espd_integration/include/bsp/**:

| Header | Implement when the board has… |
|--------|-----------------------------|
| **bsp_io.h** | WS2812/LED, physical buttons, SD slot |
| **bsp_audio.h** | Legacy void **bsp_audio_init()** (only if not using esp-bsp headers in **espd_audio_codec.c**) |

Return **ESP_ERR_NOT_SUPPORTED** for missing peripherals. No **espd_*** symbols
in the BSP.

Register in **components/bsp_myboard/Kconfig** with **choice ESPD_BOARD** and
**select** profile rules. Link from **main/CMakeLists.txt** via **REQUIRES**
when that board is selected.

## Switching boards

Configure first, then build (**idf.py** subcommands chain left to right).

**Everyday switch:**

```bash
idf.py menuconfig build flash monitor
```

**ESPD Configuration → Target board** → pick board → **Save** → exit.

**Different SoC / clean slate:**

```bash
idf.py set-target esp32s3 fullclean menuconfig build flash monitor
```

For **esp32s3**, **sdkconfig.defaults.esp32s3** applies Waveshare as the default
board reference. Profile tuning merges from
**components/espd_bsp_selector/sdkconfig.defaults** when Waveshare is selected.

**Stale sdkconfig** after a board switch: delete **sdkconfig**, then
**set-target** + **menuconfig** + **build**. Verify **CONFIG_SPIRAM=y** for
PSRAM boards and **Main task stack size ≥ 32768** (65536 for FFT-heavy Waveshare
patches).

Use one IDF environment per shell. Recommended:

```bash
alias idf='. ~/.espressif/tools/activate_idf_v6.0.1.sh'   # in ~/.zshrc
deactivate   # if a project (venv) is active
idf
```

Do not mix **`export.sh`**, **`(venv)`**, and **`activate_idf_v6.0.1.sh`**. After
switching Python envs, run **`idf.py fullclean`** once before **`build`**.

## Build and verify

From repo root (after **apply-pd-patches.sh** and IDF env active):

```bash
./scripts/apply-pd-patches.sh
idf.py set-target esp32s3 menuconfig build flash monitor
```

In menuconfig: **ESPD Configuration → Target board → Waveshare ESP32-S3-AUDIO**
(if not already default on esp32s3) → **Save**.

Boot log: **espd_board** optional inits; **espd_io** binds **espd/led** when
**bsp_led_count() > 0**; **espd/din/0..2** from K1/K2/K3 on Waveshare.

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
**config.txt**. Waveshare has no touch pads (buttons → **espd/din/N**). External
I2C touch panels are not supported yet.

## Moving BSP out of this repository

Waveshare already uses esp-bsp via git dependency — no vendored board tree in
ESPD. For new boards, prefer Path A (esp-bsp + selector + shim) over copying
drivers into **components/bsp_*/**.

See **components/espd_integration/README.md**.
