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
