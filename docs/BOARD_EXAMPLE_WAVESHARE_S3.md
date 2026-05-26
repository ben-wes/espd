# Worked example: Waveshare ESP32-S3-AUDIO

End-user setup (features, buttons, config layers):
**[GETTING_STARTED.md](GETTING_STARTED.md)**. This page is the Waveshare-specific
build shortcut.

One YAML file defines the board integration:

**[boards/waveshare_s3.yaml](../boards/waveshare_s3.yaml)**

CMake generates **`components/espd_board_waveshare_s3/`** on every configure
(Kconfig, `idf_component.yml`, `sdkconfig.defaults`, button map header, glue
CMakeLists). No hand-written C.

## Hardware

Waveshare AI Smart Speaker / ESP32-S3-AUDIO Board — ES8311 DAC, ES7210 mic,
TCA9555 expander, WS2812 LED ring, 3 buttons, microSD.

## esp-bsp source

[`ben-wes/esp-bsp@waveshare-s3-p4`](https://github.com/ben-wes/esp-bsp/tree/waveshare-s3-p4)
— fork with fixes ESPD relies on (48 kHz stereo default, codec bus init order,
ES8311/ES7210 MCLK config). Upstream merge is a publishing step, not an
architecture change.

## Build

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3
idf.py menuconfig                # ESPD Configuration → Target board → Waveshare → Save
idf.py build flash monitor
```

First build downloads the BSP into **managed_components/** (network required).

## USB (composite MSC + serial)

USB **OTG** exposes a **MSC drive** (`storage` partition) and **CDC serial** for
logs. Board profile sets `ESPD_USB_CONSOLE_CDC` and `ESP_CONSOLE_NONE`. **Pd
printf** (e.g. `cpu:` lines) goes to CDC; **ESP_LOG** does not unless you add a
separate hook (do not call `esp_log_set_vprintf` from inside USB init — it can
break enumeration). TinyUSB runs on **CPU0**; Pd/audio on **CPU1**.

```bash
grep -E 'ESPD_USB_CONSOLE_CDC|ESP_CONSOLE' sdkconfig
# expect CONFIG_ESPD_USB_CONSOLE_CDC=y and CONFIG_ESP_CONSOLE_NONE=y
```

**Monitor (important):** App logs are on the **OTG CDC** port, not USB-JTAG. Do not
use bare `idf.py monitor` — it may pick the wrong port and RTS-reset into download
mode.

```bash
ls /dev/cu.usb*    # note cu.usbmodem* (logs) vs cu.debug-console (flash only)
idf.py -p /dev/cu.usbmodem* monitor --no-reset
```

Press **RESET** on the board, wait ~2 s for USB composite (MSC + CDC). If the
monitor disconnects once (`Device not configured`), let it reconnect.

**Flash:** hold **BOOT**, tap **RESET**, release **BOOT** when esptool connects.

First boot may **format** `storage` (quiet for several seconds).

See [USB_MSC_AND_AUDIO.md](USB_MSC_AND_AUDIO.md) for optional copy/audio tuning.

See [tusb_composite_msc_serialdevice](https://github.com/espressif/esp-idf/tree/v6.0.1/examples/peripherals/usb/device/tusb_composite_msc_serialdevice).

## Buttons

Waveshare YAML maps physical keys to **`espd/din/0..2`** (numeric, not named):

| `espd/din/N` | Hardware (via `io.buttons`) |
|--------------|----------------------------|
| 0 | Vol up |
| 1 | Play |
| 2 | Vol down |

See [GETTING_STARTED.md — Buttons](GETTING_STARTED.md#buttons-bsp-kits).

## Sample rate

`CONFIG_ESPD_AUDIO_SAMPLE_RATE` (default 48000) and optional
`audio_sample_rate=` in **config.txt** drive both the codec and Pd's
`sys_getsr()`.

## Adding another kit

Copy **boards/waveshare_s3.yaml** → **boards/mykit.yaml**, edit `id`, `bsp`,
`features`, `profile`, and optional `io.buttons`. See [ADDING_A_BOARD.md](ADDING_A_BOARD.md).
