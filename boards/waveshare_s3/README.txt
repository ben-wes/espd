Waveshare ESP32-S3 AI Smart Speaker / ESP32-S3-AUDIO-Board
==========================================================

Official wiki (pins, ES8311, ES7210):
https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board

This tree uses ESP-IDF + the Component Manager package **esp_codec_dev** for
ES8311 (not the full Espressif ADF). I2C 10/11, I2S MCLK/BCLK/LRCK/DOUT/DIN
12/13/14/16/15 match the wiki “SPEAKER” table.

Build (from repo root, with IDF export.sh already sourced)
------------------------------------------------------------

  export ESPD_BOARD=waveshare_s3
  idf.py set-target esp32s3
  idf.py fullclean
  idf.py build

The top-level CMakeLists.txt merges **sdkconfig.defaults** and
**boards/waveshare_s3/sdkconfig.defaults** when ESPD_BOARD=waveshare_s3 is set in
the environment (not only on the first cmake run; delete **build/** and
**sdkconfig** if Kconfig changes seem ignored). Other boards: unset ESPD_BOARD
and keep using your existing **sdkconfig.lyrat** / **sdkconfig.wroom** workflow
(copy one to **sdkconfig** as before).

This board fragment enables octal PSRAM (per the WROVER-class S3 module on the
kit) and uses a 32 KB main task stack.

Flash / monitor (pick your USB serial port):

  idf.py -p /dev/ttyACM0 flash monitor

Audio test
----------

**main/boards/waveshare_s3/board_profile.h** enables **PD_INCLUDEPATCH**, so the
firmware runs the embedded patch from **main/testpatch.c** (dac~ + osc~ etc.)
after boot. When the board is connected, you should hear output on the
onboard speaker path if ES8311 init matches your hardware revision.

If you need stock Pd behaviour for A/B tests, use the *~_aliased objects from
**main/espdsp_osc_override.c** (see comments there).

Switching boards
----------------

Unset ESPD_BOARD or set it to another value later defined in CMakeLists.txt.
Remove **build/** and **sdkconfig** (or run **idf.py fullclean**) when changing
target or board defaults so CMake does not reuse a stale merged sdkconfig.

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
  reaches the ESP32-S3 or the **TCA9555** expander. If you find a net (e.g.
  divider into a GPIO), use **ESPD_WAVESHARE_VBUS_BACKEND_GPIO** and
  **ESPD_WAVESHARE_USB_VBUS_GPIO** in **board_profile.h** (or expander + I2C).
  If you stack **UPS HAT (E)** on the ES8311 I2C bus, use **UPS_HAT_E** instead.
- Espressif’s USB device guide (ESP32-S3) states that **self-powered** devices
  should monitor **VBUS** (comparator or resistor divider to 3.3 V-safe logic)
  and wire it to TinyUSB via **vbus_monitor_io** in **tinyusb_config_t**. That
  gives plug/unplug callbacks the stack expects. See:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html

If there is **no** usable VBUS GPIO after schematic review, fallback options
are weaker: e.g. treat **USB bus reset / enumeration** (TinyUSB “mounted”) as
“host present” and use **timeouts** when the cable is removed without VBUS
(known edge cases on some IDF versions — prefer VBUS when possible).

Software architecture (IDF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Partitioning**  
   Add a dedicated **FAT** region (flash + wear levelling, or a separate data
   partition) for the “disc” contents (e.g. **main.pd**). Extend
   **partitions_pd.csv** and Kconfig flash layout accordingly.

2. **USB stack**  
   Use the IDF **USB Device** stack (TinyUSB): MSC device pointing at the
   block device behind that FAT volume. Reference tree:
   **examples/peripherals/usb/device/tusb_msc** (SPI flash + MSC).

3. **Mutual exclusion**  
   While the host has the LUN mounted, the ESP must **not** use the same FAT
   through **esp_vfs_fat** for writes (and usually not at all until the host
   has released the volume). Typical flow:
   - **Audio mode:** mount FAT internally, optional **main.pd**, run I2S + Pd;
     **do not** expose MSC (or keep USB device off).
   - **Transition to disc:** stop DSP / Pd, **unmount** FAT on the device,
     deinit I2S if needed, start TinyUSB MSC only.
   - **Transition to audio:** stop TinyUSB / MSC, wait until stack reports
     disconnect, **remount** FAT, restart I2S + Pd.

4. **Where to branch in espd**  
   Today **app_main** in **main/espd.c** calls **pdmain_init()** then
   **initdacs()** then the main loop. A Waveshare-only path would wrap that in
   a **state machine** (e.g. FreeRTOS task + queue): **DISC** vs **AUDIO**,
   driven by VBUS / TinyUSB callbacks instead of a single linear boot.

5. **Console / download**  
   USB-Serial-JTAG and TinyUSB **share one PHY** on S3. MSC + CDC composite is
   possible but must be planned so **menuconfig** (console on USB vs UART) and
   TinyUSB descriptors stay consistent. Flashing may still use ROM USB
   download in boot mode.

6. **Hotplug testing**  
   After enabling **self_powered** and **vbus_monitor_io**, verify unplug
   triggers the expected path on your IDF version (see Espressif TinyUSB / MSC
   issues around **tud_umount_cb** and VBUS if problems appear).

Implemented today (GPIO optional)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **main/boards/waveshare_s3/waveshare_s3_usb_state.c** — optional VBUS monitoring:
  - **ESPD_WAVESHARE_VBUS_BACKEND** in **board_profile.h**: **NONE** (default),
    **GPIO** (set **ESPD_WAVESHARE_USB_VBUS_GPIO** ≥ 0), or **UPS_HAT_E** for
    Waveshare **UPS HAT (E)** on the same I2C bus as ES8311: slave **0x2D**,
    read-only register **0x02**, **bit 5 == 1** ⇒ Type-C VBUS powered (per
    Waveshare register wiki). Ephemeral I2C is used before audio init; after
    **espd_waveshare_s3_audio_init** the shared bus registers a second device at
    0x2D for polling.
  When GPIO backend is used with a valid GPIO after schematic review:
  - **Boot with VBUS present:** blocks in a **placeholder “disc” loop** until
    unplugged, then continues with normal Pd + audio init. (MSC is still TODO.)
  - **Hotplug while running audio:** debounced VBUS change calls **esp_restart()**
    so the next boot re-evaluates VBUS (simple and safe before real MSC + FAT
    teardown exists). On battery power, unplugging USB still runs the SoC so
    this path can fire.

Next step: either set **ESPD_WAVESHARE_VBUS_BACKEND** to **UPS_HAT_E** when
the UPS module shares **GPIO10/11** I2C with ES8311, or find **VBUS → GPIO**
from the S3-Audio schematic and use the **GPIO** backend, then replace the
placeholder with TinyUSB MSC + FAT per **examples/peripherals/usb/device/tusb_msc**.
