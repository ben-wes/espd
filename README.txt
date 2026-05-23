This is a very sketchy description of the "espd" version of Pd, which runs on
Espressif ESP32 bords, either generic ones (where you have to add your own
audio hardware) or LyraT or LyraT-mini boards (with built-in audio). To use
these you will almost certainly have to compile your own version, at least
either to include your own patch or to specify the WIFI settings in the file
main/espd.h .

The generic version can be used with INMP441 microphone and/or MAX98357A
DSc/amplifier.  The SPH0645 mic is known NOT TO WORK with ESP32s.

The instructions here work for me on linux; they _should_ work on macintoshes
and PCs with appropriate changes (in the shell commands for instance).

By default espd runs a built-in patch which is included as a C string defined
in the file "main/test-patch.c".  You can defeat this behavior by turing off
PD_INCLUDEPATCH in main/espd.h .

You can conditionally compile wifi or bluetooth support, which enables you to
send and receive messages and/or to send patches from a host computer. 

If you are using wifi, set up a host patch that listens on port 4498 (by
default).  When the board is booted it will connect to that port.  When you get
the connection, you can load a test patch on the esp, by sending the message "pd
begin-new poodle .", then the contents of the patch, then "pd end-new" . 
Whether you do this or rely on a pre-compiled patch, you can send messages to
any named object (such as a "receive") on the ESP32 board

To send a patch over wifi, you must compile and load espd on the board, boot the
board, and then run a patch on the host computer that waits for the board to
make a TCP connection to it.  Once connected, the host patch then sends Pd
messages to load a different patch on the esp.  The two patches then can
communicate over the same RCP connection.  There is a simple example in the
subdirectory "test-patch".

If the connection is ever broken the board reboots itself and (if WIFI is
compiled in) tries to establish a new WIFI connection.  Each time the host patch
gets a new connection it then has to reload the ESP patch.

As it stands all the vanilla Pd objects are compiled into espd except for the
FFT objects and (oddly) netsend/netreceive.  You can rebuild it with a different
choice of objects compiled in, including your own objects.

Steps to get this running: first install the espd compile chain and (for LyraT)
the "ADF" audio development platform.  The ESP documentation is excellent. 
Start here:

https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html

another (third-party) URL that might be useful:

https://gitdemo.readthedocs.io/en/latest/build-system.html

The compilation chain depends on an "sdkconfig" file.  Samples for the two LyraT
boards are included as "sdkconfig.lyrat" and "sdkconfig.lyratmini", and a sample
for a bare WROOM module is included as "sdkconfig.wroom".   You can rename one
of these as "sdkconfig" before invoking the compiler.

Per Espressif (e.g. ADF hardware reference), ESP32-LyraT V4.3 and ESP32-LyraT-Mini
V1.2 use ESP32-WROVER-E with integrated 8 MB PSRAM; the LyraT sdkconfig samples
enable SPI RAM (quad, 40 MHz) and a 32 KB main task stack for heavier Pd use.
The "sdkconfig.wroom" preset targets modules without PSRAM and keeps SPI RAM
off. "sdkconfig.bn085" enables PSRAM for WROVER-class ESP32; if your Arts board
uses a WROOM-only module, turn off PSRAM in menuconfig before flashing.

For the Waveshare ESP32-S3-AUDIO / AI Smart Speaker board, use ESP-IDF (not full
ADF) with the Component Manager codec package: in menuconfig set **ESPD
Configuration → Target board → Waveshare ESP32-S3-AUDIO** and follow
boards/waveshare_s3/README.txt. That keeps
LyraT / WROOM sdkconfig.* samples unchanged for other hardware.

In addition to the sources youre looking at you'll need Pd source code. This is
included as a git submodule:

git clone --recursive [...]

or, from an existing clone:

git submodule update --init --recursive

The superproject pins a specific Pd commit for reproducible builds. After
switching branches or pulling changes, run:

git submodule update --init --recursive

to move submodules to the commits recorded by the checked-out branch.

Then apply the required Pd compatibility patch:

./scripts/apply-pd-patches.sh

This script is idempotent and safe to re-run after submodule updates.

To compile, set up your environment variables and issue commands to compile,
flash, and run the monitor program to see debugging output (see bottom of this
page to see what I type on my system).  This should be done from a shell window
that is in this (espd) directory.

CAUTION: compiling for LyraT boards works differently from raw WROOM boards.

Commands I issue to shell to compile (customize to your own installation):

*********  For generic ESP32 modules: *********

FIRST copy sdkconfig.wroom to sdkconfig.  Then:

export IDF_TOOLS_PATH=~/bis/var/esp/tools
export IDF_PATH=~/bis/var/esp/esp-idf
. $IDF_PATH/export.sh

*********  for LyraT boards:  *********

FIRST edit Cmakelists.txt to enable esp_adf and copy one of sdkconfig.lyrat*
to sdkconfig.  Then:

export IDF_TOOLS_PATH=~/bis/var/esp/tools
export ADF_PATH=~/bis/var/esp/esp-adf
 . $ADF_PATH/esp-idf/export.sh

then (for either type of board):

(optional:) idf.py menuconfig
idf.py build
To verify the firmware without writing flash, stop after **idf.py build**.
idf.py flash
idf.py monitor

... if idf.py doesn't find your TTY port you can try, for instance:
idf.py -p /dev/ttyUSB0 flash

Waveshare quick start (ESP32-S3-AUDIO)
--------------------------------------

Target **esp32s3**, then **idf.py menuconfig → ESPD Configuration → Target board →
Waveshare ESP32-S3-AUDIO** (default in **sdkconfig.defaults**). Board-specific
IDF tuning lives in **components/bsp_waveshare_s3/sdkconfig.defaults**.

For Waveshare-specific steps, including build, flash, and monitor notes, see:

boards/waveshare_s3/README.txt
