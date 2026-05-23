Waveshare ESP32-S3 AI Smart Speaker / ESP32-S3-AUDIO-Board
==========================================================

Official wiki (pins, ES8311, ES7210):
https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board

This tree uses ESP-IDF + the Component Manager package **esp_codec_dev** for
ES8311 (not the full Espressif ADF). I2C 10/11, I2S MCLK/BCLK/LRCK/DOUT/DIN
12/13/14/16/15 match the wiki “SPEAKER” table.

Build (from repo root)
------------------------------------------------------------

Set up the toolchain the same way as in the top-level **README.md** (Espressif
getting-started guide): point **IDF_PATH** (and optionally **IDF_TOOLS_PATH**)
at your ESP-IDF tree, then source **export.sh** so **idf.py** is on **PATH**.
If you use **ESP-ADF**, its bundled IDF works too, for example:

  export ADF_PATH=/path/to/esp-adf
  . "$ADF_PATH/esp-idf/export.sh"

Then (Waveshare — this repository's reference board):

  idf.py set-target esp32s3 menuconfig build flash monitor

**sdkconfig.defaults.esp32s3** at the repo root applies the full Waveshare tune
(PSRAM oct 80 MHz, 240 MHz, 65536 main stack, CPU affinities) on **set-target
esp32s3** — no manual Component config tuning needed. Pick **Target board →
Waveshare** in menuconfig if it is not already selected, Save, then build.

**CONFIG_ESPD_BSP_COMPONENT_NAME** in **sdkconfig** selects the linked BSP; no
extra **-D** flags are required.

Generic I2S is the menuconfig default when no Waveshare board is selected.

**idf.py build** alone is enough to verify the firmware compiles; you do not
need to flash.

Repository / submodule sync
---------------------------

From a fresh clone, or after switching branches, update submodules to the
commits pinned by the checked-out branch:

  git submodule update --init --recursive

This project pins a specific Pd submodule commit for reproducible firmware
builds.

The top-level CMakeLists.txt merges **sdkconfig.defaults** plus one board profile
(**main/boards/generic/** or **components/bsp_*/**). After you select this board
in menuconfig and Save, Kconfig applies the full tune (PSRAM, 240 MHz, CPU
affinities). If **sdkconfig** predates the board switch, delete it and run
**idf.py menuconfig build** so PSRAM and other IDF keys refresh — see
*Switching boards* below.

If **idf.py build** fails at the end with **app partition is too small**, your
**sdkconfig** was probably created before this board picked **partitions_pd.csv**
(**CONFIG_PARTITION_TABLE_CUSTOM**). Remove **sdkconfig** once and rebuild so
defaults merge again (factory app slot is **1536K** in **partitions_pd.csv**).

This board fragment enables octal PSRAM (per the WROVER-class S3 module on the
kit) and uses a **65536** byte main task stack. Pd FFT patches require PSRAM;
if boot logs **getbytes() failed** or **SPIRAM is off**, delete **sdkconfig**
and reconfigure (see *Switching boards* below).

Performance / memory profile
----------------------------

Current defaults are tuned for real-time Pd use:

- compiler optimization favors speed (`CONFIG_COMPILER_OPTIMIZATION_PERF`)
- ESP-IDF logs default to WARN to reduce UART overhead (Pd `[print]` still works)
- websocket and OpenThread are disabled for lower baseline footprint
- only ES8311/ES7210 codec targets are enabled
- WiFi/LWIP buffer counts are reduced and selected allocations are allowed in PSRAM

Watchdog rationale:

- the watchdog is a safety net for deadlocks/hangs, not a speed feature
- for this firmware, monitoring is tuned so heavy DSP bursts on CPU0 do not trigger
  false positives, while watchdog coverage remains active
- keep it enabled unless you are explicitly running bring-up experiments

Flash / monitor (pick your USB serial port)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

On macOS the port is usually **/dev/cu.usbmodem*** (use **cu.**, not **tty.**,
for flashing). On Linux it is often **/dev/ttyACM0**.

  idf.py -p /dev/cu.usbmodemXXXX flash monitor

Serial monitor: UART vs USB (why logs “stop” after boot)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Merged **sdkconfig** defaults use **UART0** as the **primary** console (this
target often binds UART0 to **GPIO43/44** — see the **cpu_start** line in the
boot log). Application **ESP_LOGI** output goes there, not necessarily to
**/dev/cu.usbmodem***.

If **idf.py monitor** on **usbmodem** prints ROM / second-stage lines then goes
quiet right after “Loaded app” / “Disabling RNG…”, you are still on USB while
the firmware is talking on **UART**:

1. Use a **3.3 V USB–UART** on the board’s **UART0** pins named in that boot line
   (adapter **RX** → ESP **TX**, adapter **TX** → ESP **RX**, **GND** common;
   confirm TX/RX pins on the Waveshare wiki / schematic), and run **idf.py -p
   /dev/cu.usbserial-… monitor**, or

2. Run **idf.py menuconfig** → **Component config → ESP System Settings →
   Channel for console output** → set primary console to **USB Serial/JTAG**,
   rebuild, flash, then **usbmodem** will show app logs end-to-end.

**esptool: “No serial data received” / “Failed to connect”**

1. **Confirm the port** — Unplug the board, list serial-style **cu** devices,
   plug the board in again, and flash the path that **newly appears**. On **zsh**,
   a command like **ls /dev/cu.wchusbserial*** errors if that pattern matches
   nothing; use one of these instead:

     ls /dev/cu.* 2>/dev/null | grep -E 'usbmodem|wchusb|SLAB|usbserial'

   or (zsh only, optional globs):

     print -rl /dev/cu.usbmodem*(N) /dev/cu.wchusbserial*(N) /dev/cu.SLAB*(N)

   If several **usbmodem** devices exist, pick the one tied to this board (a
   name like **usbmodem1234561** is often **not** the Espressif device).

2. **Manual download mode** — Hold **BOOT**, tap **RESET**, release **BOOT**,
   then run **idf.py flash** within a few seconds (same USB port the ROM uses).

3. **Slower baud** — **idf.py -p … flash -b 115200** (or **460800**) can help
   on long or marginal cables.

4. **Power / cable** — Use a **data** USB-C cable, try another host port or a
   **powered hub**. If a battery is fitted, disconnect it once so the chip gets a
   clean USB-only power cycle.

5. **UART fallback** — This kit’s main connector is native USB on the S3; if
   the USB-serial-JTAG path stays broken, use any **3.3 V UART** wired to the
   module’s **TX/RX/GND** (see wiki for test points or headers) and flash with
   **idf.py -p /dev/cu.usbserial-… flash** for that adapter.

Audio test
----------

**menuconfig** (**ESPD Configuration**) enables **PD_INCLUDEPATCH** on the Waveshare
board by default, so the
firmware runs the embedded patch from **main/testpatch.c** (dac~ + osc~ etc.)
after boot **unless** a file **main.pd** exists on the SPIFFS patch store.

**main.pd on SPIFFS (overrides embedded patch)**
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **partitions_pd.csv** adds a **512K** SPIFFS partition labeled **pdstore**,
  mounted at **`/espd_pd`** at boot (see **main/espd_storage.c**). SD card
  **`/sdcard`** is tried first for **main.pd** and **config.txt**; SPIFFS is the
  fallback when the SD card is missing or has no files.
- If **`/espd_pd/main.pd`** is present, it is opened with Pd’s normal file
  loader (**glob_evalfile**); the embedded **testpatch.c** patch is **not** run.
- Populate SPIFFS with Espressif’s **SPIFFS image generation** tools (see the
  ESP-IDF “SPIFFS” docs): build an image containing **main.pd** and flash the
  **pdstore** region, or use **format_if_mount_failed** (enabled) on a fresh
  chip and flash a prebuilt SPIFFS binary at the **pdstore** offset from the
  partition table.
- **ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK** in menuconfig (default **on** for Waveshare)
  skips **wifi_init** / **net_init** / **net_hello** when **main.pd** was loaded
  from SPIFFS so the board does not join WiFi or wait for host-sent patches.
  Set it to **off** in menuconfig if you want WiFi + TCP/UDP patch
  transport even with a local **main.pd**.

SD card runtime WiFi config (optional)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When **PD_USE_WIFI** and **PD_USE_SDCARD** are enabled, firmware reads
**config.txt** (SD → SPIFFS) at boot. STA connects when **wifi_ssid** is
non-empty:

  wifi_ssid=YourNetwork
  wifi_password=YourPassword

Optional **wifi_enable** (only needed for overrides):
- **wifi_enable=0** — disable STA even if **wifi_ssid** is set
- **wifi_enable=1** — force STA when **ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK**
  would otherwise skip WiFi

Ports are not configured in this file; use Pd **netsend** / **netreceive**.

If you need stock Pd behaviour for A/B tests, use the *~_aliased objects from
**main/espdsp_osc_override.c** (see comments there).

Switching boards
----------------

Same flow as any BSP — see **docs/ADDING_A_BOARD.txt** (*Switching boards*):

```
idf.py menuconfig build flash monitor
```

**ESPD Configuration → Target board** → pick this board or **Generic I2S** → Save.
Board-locked options show as `-*-` in menuconfig (e.g. **espd/ain**, **espd/aout** compile
support). Runtime GPIOs still come from **config.txt** (**ain_pins=**, **aout_pins=**, …).

If the profile looks wrong after a switch, delete **sdkconfig** and reconfigure
with **set-target** in the chain (this kit is **esp32s3**, not **esp32**):

```
idf.py set-target esp32s3 fullclean menuconfig build flash monitor
```

Pick **Target board** → Save. Without **set-target**, a fresh configure defaults
to **esp32** and the build fails on S3-only components (**esp_tinyusb**, PSRAM, …).

USB “disc” vs audio + Pd (hotplug, future work)
-----------------------------------------------

Goal: when USB is connected to a host, behave as a USB mass-storage volume
(FAT on flash); when USB is unplugged, run audio + Pd. Prefer **no reset
button** — that implies **reliable plug / unplug detection** and **clean
teardown** of one stack before starting the other.

Hardware (Waveshare ESP32-S3-AUDIO-Board)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- The wiki lists a single **USB Type-C** for power and flashing (native S3
  D+/D−, not a separate UART bridge). There is also **battery** power — i.e.
  the board can run **without** USB, which matches a **self-powered** USB
  device model.
- The public pin tables **do not** name a **VBUS sense** GPIO. For **hotplug**
  without guessing, open the **official schematic** (linked from the wiki) and
  check whether **VBUS** (or a USB power-detect line from the Type-C front-end)
  reaches the ESP32-S3 or the **TCA9554/TCA9555** I²C expander. Tune expander
  levels under **menuconfig → Board Support Package (Waveshare S3)** if USB
  routing or the speaker amp needs adjustment.
- Espressif’s USB device guide (ESP32-S3) states that **self-powered** devices
  should monitor **VBUS** (comparator or resistor divider to 3.3 V-safe logic)
  and wire it to TinyUSB via **vbus_monitor_io** in **tinyusb_config_t**. That
  gives plug/unplug callbacks the stack expects. See:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html

If there is **no** usable VBUS GPIO after schematic review, fallback options
are weaker: e.g. treat **USB bus reset / enumeration** (TinyUSB “mounted”) as
“host present” and use **timeouts** when the cable is removed without VBUS
(known edge cases on some IDF versions — prefer VBUS when possible).

Software architecture
---------------------

**main/Kconfig.projbuild** is board-agnostic: **Generic I2S** is the default
choice. The Waveshare option is registered by **components/bsp_waveshare_s3/Kconfig**
(choice member + **imply** profile + hardware menu).

Select **ESPD Configuration → Target board → Waveshare ESP32-S3-AUDIO** in
menuconfig. **CONFIG_ESPD_BSP_COMPONENT_NAME** links the BSP; **sdkconfig.defaults**
from this component merges on the next configure.

- **components/bsp_waveshare_s3/** — I2C, TCA9555, I2S, ES8311/ES7210, LED,
  buttons, SD card (upstream-safe **bsp_*** only).
- **components/espd_integration/** — optional **bsp_*** contract + weak stubs.
- **main/espd_board.c** — probes linked **bsp_*** drivers (no board names).
- **main/espd_io.c** — generic Pd **espd/din** / **espd/led** bindings.
- **main/espd_audio_codec.c** — generic codec backend for **dac~** / **adc~**.
- **main/boards/generic/sdkconfig.defaults** — profile when Generic is selected.

After switching boards, delete **sdkconfig** and run **idf.py build** again.

Implemented today (audio + expander)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **bsp_waveshare_s3** — probes the I²C expander on **GPIO10/11** (addresses
  **0x20–0x27**). **TCA9555** (16 GPIO): drives **EXIO6/EXIO7** for Type-C
  **D+/D−** routing to **GPIO19/20** and **port1** bits for the **NS4150**
  speaker enable. Tune via **menuconfig → Board Support Package (Waveshare S3)**:
  **EXIO7 USB route level**, **EXIO6 camera select level**, **PA port1 mask**.
- **bsp_audio_init()** — ES8311 playback and (when enabled) ES7210 capture at
  48 kHz stereo; used by **espd_audio_init()** before the Pd main loop.
- **UART console** (USB-Serial-JTAG disabled in defaults) because TinyUSB and
  JTAG share the internal USB PHY on ESP32-S3.

Input: three physical buttons → **espd/din/0..2** (TCA9555, not capacitive
touch). Optional GPIO digital ins append after those indices (**din_pins=** in
**config.txt**, **ESPD_PD_USE_DIN0**); e.g. **din_pins=4,5** → **espd/din/3..4**
when three BSP buttons are present. Boot log prints the full map. This kit has
no on-board touch pads; for **espd/touch** wire external electrodes to free
touch GPIOs (1–14), enable **ESPD_PD_USE_TOUCH0**, and set **touch_pins=** in
**config.txt** — see **docs/ADDING_A_BOARD.txt**.

USB MSC / VBUS hotplug disc mode is still planned; see the sections above for
design notes. Next step: TinyUSB MSC + FAT per
**examples/peripherals/usb/device/tusb_msc**.
