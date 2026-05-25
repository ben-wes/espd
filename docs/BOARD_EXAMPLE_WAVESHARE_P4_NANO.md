# Worked example: Waveshare ESP32-P4-NANO

End-user setup: **[GETTING_STARTED.md](GETTING_STARTED.md)**. This page covers
P4-NANO specifics: ESP-Hosted Wi-Fi and build.

Board definition: **[boards/waveshare_p4_nano.yaml](../boards/waveshare_p4_nano.yaml)**

## Hardware

- **ESP32-P4** — application MCU (audio, Pd, SD, optional display via BSP).
- **ESP32-C6-MINI** — onboard Wi-Fi 6 / BLE coprocessor, **SDIO** to the P4
  (same SDIO pinout as Espressif’s P4 Function EV board; Waveshare NANO uses
  CLK 18, CMD 19, D0–D3 14–17, C6 reset 54).
- **ES8311** codec, microSD (same espd audio path as S3 BSP boards).

## Wi-Fi (ESP-Hosted)

The P4 has no native Wi-Fi. The **esp32_p4_nano** BSP declares **esp_wifi_remote**
and **esp_hosted** (transitive via the board plugin’s BSP dep). Your application
still calls `esp_wifi_*`; the stack talks to the C6 over SDIO.

**Runtime (same as S3):** put credentials on the SD card:

```text
wifi_ssid=your_ap
wifi_password=your_secret
```

If `wifi_ssid` is missing or empty, STA stays off (see boot log).

**menuconfig (first P4 build):** Component config → **Wi-Fi Remote** → slave
target **esp32c6**. **ESP-Hosted config** → transport **SDIO**. Board
`sdkconfig.defaults` already sets `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`
and C6 target symbols.

### C6 slave firmware

The NANO is usually shipped with ESP-Hosted slave firmware on the C6. If SDIO
init fails at boot (no “transport: Base transport is set-up” in the log),
update the C6 image via Waveshare’s **PROG_C6** header (ESP-Prog) or ESP-Hosted
[slave OTA](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_performs_slave_ota).
See [esp-hosted P4 Function EV guide](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md).

Do **not** add `espressif/esp-extconn` on P4 — it conflicts with esp_hosted.

## esp-bsp source

[`ben-wes/esp-bsp@waveshare-s3-p4`](https://github.com/ben-wes/esp-bsp/tree/waveshare-s3-p4)
— `bsp/esp32_p4_nano` (IDF 6, audio/SD; display optional via BSP).

## Build

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32p4
idf.py menuconfig                # ESPD Configuration → Waveshare P4 Nano → Save
idf.py build flash monitor
```

First build downloads BSP, **esp_hosted**, and **esp_wifi_remote** into
`managed_components/` (network required).

## Audio

Same BSP codec path as other esp-bsp boards: `config.txt` may set
`audio_sample_rate=` (default 48000 Hz). No `aout_pins=` / `ain_pins=` needed
for the ES8311 path.
