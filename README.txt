This is a very sketchy description of the "espd" version of Pd, which runs on
Espressif ESP32 bords, eoither generic ones (where you have to add your own
audio hardware) or LyraT or LyraT-mini boards (with built-in audio). To use
these you will almost certainly have to compile your own version, at least
either to include your own patch or to specify the WIFI settings in the file
main/espd.h .

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
any named object (such as a "receive") on the ESP32 board.

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

The compilation chain depends on an "sdkconfig" file.  The included one is for a
generic ESP board. Samples for the two LyraT boards are included as
"sdkconfig.lyrat" and "sdkconfig.lyratmini" - you can rename one of these as
"sdkconfig" before invoking the compiler (and compile using the "ADF", not the
"IDF" - see below).

In addition to the sources youre looking at you'll need Pd, preferably the
latest version, although I'm testing this with Pd commit
177350fc4999b74ea28a12ba2981baa6ae04c6f0 (0.55-1 with a couple of tweaks added).
This is included as a git submodule ("git clone --recursive [...]") .  Or
you can just copy the pd source into a subdirectory "pd" of this directory.

Then you must apply three small patches to the Pd source, found in the
subdirectory "patches".

To compile, set up your environment variables and issue commands to compile,
flash, and run the monitor program to see debugging output (see bottom of this
page to see what I type on my system).  This should be done from a shell window
that is in this (espd) directory.

Commands I issue to shell to compile (customize to your own installation):

For generic ESP32 modules:

export IDF_TOOLS_PATH=~/bis/var/esp/tools
export IDF_PATH=~/bis/var/esp/esp-idf
. $IDF_PATH/export.sh

for LyraT boards:

### FIRST edit Cmakelists.txt to enable esp_adf ###
export IDF_TOOLS_PATH=~/bis/var/esp/tools
export ADF_PATH=~/bis/var/esp/esp-adf
 . $ADF_PATH/esp-idf/export.sh

then (for either type of board):

(optional:) idf.py menuconfig
idf.py build
idf.py flash
idf.py monitor

... if idf.py doesn't find your TTY port you can try, for instance:
idf.py -p /dev/ttyUSB0 flash
