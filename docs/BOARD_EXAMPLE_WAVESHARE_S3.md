# Worked example: Waveshare ESP32-S3-AUDIO

End-user setup (features, buttons, config layers):
**[README.md](../README.md)**. This page is the Waveshare-specific build shortcut.

One YAML file defines the board integration:

**[boards/waveshare_s3.yaml](../boards/waveshare_s3.yaml)**

CMake generates **`components/espd_board_waveshare_s3/`** on every configure
(Kconfig, `idf_component.yml`, `sdkconfig.defaults`, button map header, glue
CMakeLists). No hand-written C.

## Hardware

Waveshare AI Smart Speaker / ESP32-S3-AUDIO Board — ES8311 DAC, ES7210 mic,
TCA9555 expander, WS2812 LED ring, 3 buttons, microSD.

## esp-bsp source

[`ben-wes/esp-bsp@espd-bsp`](https://github.com/ben-wes/esp-bsp/tree/espd-bsp)
— fork with fixes ESPD relies on (48 kHz stereo default, codec bus init order,
ES8311/ES7210 MCLK config). Upstream merge is a publishing step, not an
architecture change.

## Build

**Pick the Waveshare board in menuconfig** — until you do, the base 4 MB fallback
applies. Selecting the board then applies its `flash: 16MB`, for which the generator
emits a partition table (factory fixed, storage = remainder) overriding the fallback.

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3
idf.py menuconfig                # ESPD Configuration → Target board → Waveshare → Save
idf.py build flash monitor
```

Check: `grep CONFIG_ESPD_BOARD_WAVESHARE_S3=y sdkconfig`

First build downloads the BSP into **managed_components/** (network required).

Board YAML turns on OTG (CDC + MSC + patch sync). Chip defaults also set 4 KiB
wear-levelling and MSC buffer sizes for `/storage`. Patches, flash, and
`espd_sync.py`: **[DEV_SYNC.md](DEV_SYNC.md)**.

Prebuilt firmware: **[espd-kits](https://github.com/ben-wes/espd-kits)** (optional).

**After upgrading** from 512-byte WL: reformat `/storage` once — see
**[DEV_SYNC.md — Reformat internal flash](DEV_SYNC.md#reformat-internal-flash-storage)**.

**Flash clock (optional):** menuconfig → Serial flasher config → 120 MHz + HPM
only if the module supports it; if the board fails to boot, revert to 80 MHz.

## Buttons

Waveshare YAML maps physical keys to **`espd/din/0..2`** (numeric, not named):

| `espd/din/N` | Hardware (via `io.buttons`) |
|--------------|----------------------------|
| 0 | Vol up |
| 1 | Play |
| 2 | Vol down |
