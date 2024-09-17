This is a very sketchy description of the "espd" version of Pd, which runs on
Espressif ESP32 bords, eoither generic ones (where you have to add your own
audio hardware) or LyraT or LyraT-mini boards (with built-in audio). To use
these you will almost certainly have to compile your own version, at least
eitehr to include your own patch or to specify the WIFI settings in the file
main/espd.h .

The instructions here work for me on linux; they _should_ work on macintoshes
and PCs with appropriate changes (in the shell commands for instance).

To send a patch over wifi, you must compile and load espd on the board, boot the
board, and then run a patch on the host computer that waits for the board to
make a TCP connection to it.  Once connected, the host patch then sends Pd
messages to load a different patch on the esp.  The two patches then can
communicate over the same RCP connection.

If the patch is built-in you can skip the previous step.  You can still compile
in wifi if you want and send Pd messages to the board over it.

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
"sdkconfig" before invoking the compiler.

In addition to the sources youre looking at you'll need Pd, preferably the
latest version, although I'm testing this with Pd commit
177350fc4999b74ea28a12ba2981baa6ae04c6f0 (0.55-1 with a couple of tweaks added).
You can "git clone" pd into a subdirectory "pd" of this directory.

Then you must apply three small patches to the Pd source, found in the
subdirectory "patches".

To compile, set up your environment variables and issue commands to compile,
flash, and run the monitor program to see debugging output (see bottom of this
page to see what I type on my system).  This should be done from a shell window
that is in this (espd) directory.

Then, if you are using wifi, set up a host patch that listens on port 4498 (by
default).  When the board is booted it will connect to that port.  When you get
the connection, you can load a test patch on the esp, by sending the message "pd
begin-new poodle .", then the contents of the patch, then "pd end-new" .  Whether
you do this or rely on a pre-compiled patch, you can send messages to any named object (such as a "receive") on the ESP32 board.

Commands I issue to shell to compile (customize to your own installation):

For generic ESP:

export IDF_TOOLS_PATH=~/bis/var/esp/tools
export IDF_PATH=~/bis/var/esp/esp-idf
. $IDF_PATH/export.sh

for LyraT boards:

### FIRST edit Cmakelists.txt to enable esp_adf ###
export IDF_TOOLS_PATH=~/bis/var/esp/tools
export ADF_PATH=~/bis/var/esp/esp-adf
 . $ADF_PATH/esp-idf/export.sh

then (for either type of board):

idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
