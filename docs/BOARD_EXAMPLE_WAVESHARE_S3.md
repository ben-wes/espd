# Worked example: Waveshare ESP32-S3-AUDIO

One YAML file defines the whole board integration:

**[boards/waveshare_s3.yaml](../boards/waveshare_s3.yaml)**

CMake generates **`components/espd_board_waveshare_s3/`** on every configure
(Kconfig, `idf_component.yml`, `sdkconfig.defaults`, button map header, glue
CMakeLists). No hand-written C.

## Hardware

Waveshare AI Smart Speaker / ESP32-S3-AUDIO Board — ES8311 DAC, ES7210 mic,
TCA9555 expander, WS2812 LED ring, 3 buttons, microSD.

## esp-bsp source

[`ben-wes/esp-bsp@waveshare-bsp`](https://github.com/ben-wes/esp-bsp/tree/waveshare-bsp)
— fork with fixes ESPD relies on (48 kHz stereo default, codec bus init order,
ES8311/ES7210 MCLK config). Upstream merge is a publishing step, not an
architecture change.

## Build

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3
echo CONFIG_ESPD_BOARD_WAVESHARE_S3=y >> sdkconfig.defaults.esp32s3
idf.py menuconfig          # save — board appears under Target board
idf.py build flash monitor
```

First build downloads the BSP into **managed_components/** (network required).

## Verifying the BSP fork

```bash
grep -n 'I2S_STD_PHILIP[^S]'                     managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'bsp_audio_codec_bus_init'               managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'sample_rate.*22050\|sample_rate.*48000' managed_components/waveshare_esp32_s3_audio/src/*.c
```

## Sample rate

`CONFIG_ESPD_AUDIO_SAMPLE_RATE` (default 48000) and optional
`audio_sample_rate=` in **config.txt** drive both the codec and Pd's
`sys_getsr()`.

## Adding another kit

Copy **boards/waveshare_s3.yaml** → **boards/mykit.yaml**, edit `id`, `bsp`,
`features`, `profile`, and optional `io.buttons`. See [ADDING_A_BOARD.md](ADDING_A_BOARD.md).
