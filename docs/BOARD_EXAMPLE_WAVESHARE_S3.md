# Worked example: Waveshare ESP32-S3-AUDIO

A complete `espd_board_waveshare_s3` plugin for the **Waveshare AI Smart Speaker
/ ESP32-S3-AUDIO Board** (ES8311 DAC, ES7210 mic, TCA9555 expander, WS2812
LED ring, 3 buttons, microSD).

It uses [`ben-wes/esp-bsp@waveshare-bsp`](https://github.com/ben-wes/esp-bsp/tree/waveshare-bsp),
a fork of [`espressif/esp-bsp`](https://github.com/espressif/esp-bsp) with the
fixes ESPD relies on:

- 48 kHz **stereo** I²S default (matches Pd's `sys_getsr() = 48000`; upstream
  defaults to 22 050 Hz mono)
- `bsp_audio_codec_bus_init()` — always brings up I²C + TCA9555 expander
  before codec init, even when I²S was pre-configured via `bsp_audio_init()`
- ES8311 `use_mclk` / `mclk_div`; ES7210 dual-mic + MCLK config
- `I2S_STD_PHILIP` → `I2S_STD_PHILIPS` typo

The same recipe works for any other esp-bsp kit by swapping the package
name and shim — see [ADDING_A_BOARD.md](ADDING_A_BOARD.md) for the general
contract.

## Files to create

```
components/espd_board_waveshare_s3/
├── Kconfig.board
├── idf_component.yml
├── CMakeLists.txt
├── sdkconfig.defaults
├── waveshare_io.c        # recover from git history (see below)
└── waveshare_audio.c     # recover from git history (see below)
```

The two C shim files are large enough that recovery from git is preferable to
inlining them. Commit `79e9f9c` is the last revision of this repo that shipped
the in-tree plugin:

```bash
mkdir -p components/espd_board_waveshare_s3
cd components/espd_board_waveshare_s3
git show 79e9f9c:components/espd_board_waveshare_s3/waveshare_io.c    > waveshare_io.c
git show 79e9f9c:components/espd_board_waveshare_s3/waveshare_audio.c > waveshare_audio.c
cd -
```

The four metadata files below are short and should be created by hand so they
match the current ESPD APIs.

### `Kconfig.board`

```kconfig
config ESPD_BOARD_WAVESHARE_S3
    bool "Waveshare ESP32-S3-AUDIO"
    depends on IDF_TARGET_ESP32S3
    imply ESPD_USE_ADC
    imply ESPD_PD_USE_SDCARD
    imply ESPD_PD_USE_ANALOG0
    imply ESPD_PD_INCLUDEPATCH
    imply ESPD_PD_USE_AOUT
    imply ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK
    help
        Waveshare AI Smart Speaker / ESP32-S3-AUDIO Board (ES8311 DAC,
        ES7210 mic). Fetches esp-bsp via Component Manager.
```

### `idf_component.yml`

`espd_integration` is pulled in via `path:` (transitively brings
`espressif/esp_codec_dev` for the codec backend). The BSP itself is fetched
from the `ben-wes` fork:

```yaml
version: "0.1.0"
description: Waveshare ESP32-S3-AUDIO board plugin for espd
dependencies:
  idf: ">=6.0.1,<6.1"
  espd_integration:
    path: ../espd_integration
  waveshare_esp32_s3_audio:
    git: https://github.com/ben-wes/esp-bsp.git
    path: bsp/waveshare_esp32_s3_audio
    version: waveshare-bsp
```

### `CMakeLists.txt`

No `REQUIRES` — every dep flows through `idf_component.yml` (works around an
IDF v6 quirk where managed `reqs` would otherwise replace local CMake
`REQUIRES` for components added via `EXTRA_COMPONENT_DIRS`).

```cmake
if(CONFIG_ESPD_BOARD_WAVESHARE_S3)
    idf_component_register(
        SRCS "waveshare_io.c" "waveshare_audio.c"
    )
else()
    idf_component_register()
endif()
```

### `sdkconfig.defaults`

ESPD feature flags + IDF tuning that the Waveshare hardware needs (16 MB
flash, octal PSRAM, codec drivers, expander GPIO API, CPU/network task
affinity for Pd).

```ini
# --- ESPD features ---
CONFIG_ESPD_USE_ADC=y
CONFIG_ESPD_PD_USE_SDCARD=y
CONFIG_ESPD_PD_USE_ANALOG0=y
CONFIG_ESPD_PD_INCLUDEPATCH=y
CONFIG_ESPD_PD_USE_AOUT=y
CONFIG_ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK=y

# --- Board hardware (kit requirements) ---
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536

CONFIG_ESP32S3_REV_MIN_2=y
CONFIG_ESP32S3_REV_MIN_FULL=2
CONFIG_ESP_REV_MIN_FULL=2

CONFIG_CODEC_ES8311_SUPPORT=y
CONFIG_CODEC_ES7210_SUPPORT=y

CONFIG_BSP_SD_MOUNT_POINT="/sdcard"
CONFIG_IO_EXPANDER_ENABLE_GPIO_API_WRAPPER=y

# --- Pd runtime tuning (CPU affinity, stacks, WiFi/LwIP for netsend) ---
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1=y
CONFIG_ESP_MAIN_TASK_AFFINITY=0x1
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY=0x0
CONFIG_ESP_TIMER_TASK_AFFINITY=0x0
CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y

CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=8192
CONFIG_PTHREAD_TASK_PRIO_DEFAULT=5
CONFIG_PTHREAD_TASK_CORE_DEFAULT=0

CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16
CONFIG_LWIP_MAX_SOCKETS=8
CONFIG_LWIP_IPV6=y
CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y

CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=0

CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_DEFAULT_LEVEL=2
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL=3

CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART=y
CONFIG_ESP_CONSOLE_UART_NUM=0

CONFIG_FREERTOS_IN_IRAM=y
```

## Build

From the ESPD repo root, after creating the plugin folder above:

```bash
. $HOME/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3
echo CONFIG_ESPD_BOARD_WAVESHARE_S3=y >> sdkconfig.defaults.esp32s3
idf.py menuconfig          # save without changes — picks up the line above
idf.py build flash monitor
```

The first build downloads `ben-wes/esp-bsp` into `managed_components/`
(network required). Pin it for CI by committing **`dependencies.lock`**.

## Verifying the BSP fork

If the codec is silent or pitched wrong, your BSP checkout may predate the
samplerate / init-order fixes listed at the top of this file. Confirm:

```bash
grep -n 'I2S_STD_PHILIP[^S]'                     managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'bsp_audio_codec_bus_init'               managed_components/waveshare_esp32_s3_audio/src/*.c
grep -n 'sample_rate.*22050\|sample_rate.*48000' managed_components/waveshare_esp32_s3_audio/src/*.c
```

The first should return nothing (typo fixed), the second should resolve to a
function definition, the third should show 48000 as the default.

## Pd sample rate

Pd's `sys_getsr()` returns 48000 (see [`main/pdmain.c`][pdmain]). ESPD's
audio backends ask the codec/I²S for the same rate. The `ben-wes/esp-bsp`
fork above respects the requested rate; if you swap in another fork or
the upstream registry version, double-check the I²S default in
`managed_components/waveshare_esp32_s3_audio/` and adjust accordingly.

A future ESPD release will make sample rate configurable via `config.txt`
(`audio_sample_rate=`) so the contract is explicit instead of implicit.

[pdmain]: ../main/pdmain.c
