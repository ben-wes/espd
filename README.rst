Commands I issue to shell to compile (customize to your own installation):

export IDF_TOOLS_PATH=/home/msp/bis/work/esp/toolchain
export ADF_PATH=/home/msp/bis/work/esp/esp-adf
export IDF_PATH=/home/msp/bis/work/esp/esp-idf-v4.4.2
. $ADF_PATH/esp-idf/export.sh

idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyUSB0 monitor

**** START READING HERE ****

This is a very sketchy description of the "espd" version of Pd, which runs
on Espressif LyraT or LyraT-mini boards.  To use these you will almost certainly
have to compile your own version, at least to specify the WIFI settings which
are set in the file main/espd.h .

The instructions here _should_ work on macintoshes and PCs with appropriate
changes (in the shell commands for instance).  In addition to a linux machine
you'll need an Espressif LyraT or LyraT mini board.

To run espd, you must compile and load it on the board, biit the board, and then
run a patch on the host computer that waits for the board to make a TCP
connection to it.  Once connected, the host patch then sends Pd messages to
load a different patch on the esp.  The two patches then can communicate over
the same RCP connection.

If the connection is ever broken the board reboots itself and tries to establish
a new connection.  Each time the host patch gets a new connection it then has to
reload the ESP patch.

As it stands all the vanilla Pd objects are compiled into espd except for the
FFT objects and (oddly) netsend/netreceive.  You can rebuild it with a different
xhoice of objects compiled in, including your own objects.

Steps to get this running: first install the espd compile chain and the
"ADF" audio development platform.  The ESP documentation is excellent.  Start
here:

https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html

another (third-party) URL that might be useful:

https://gitdemo.readthedocs.io/en/latest/build-system.html

The compilation chain depends on an "sdkconfig" file - samples for the two
boards are included as "sdkconfig.lyrat" and "sdkconfig.lyratmini" - you can
rename one of these as "sdkconfig" before invoking the compiler.

In addition to the sources youre looking at you'll need Pd, preferably the
latest version, although I'm testing this with Pd commit
05bf346fa32510fd191fe77de24b3ea1c481f5ff .  You can "git" clone pd into a
subdirectory "pd" of this directory.

Then you must apply four small patches to the Pd source, found in the
subdirectory "patches".

To compile, set up your envoronment variables and issue commands to compile,
flash, and run the monitor program to see debugging output (see top of this page
to see what I typw on my system).  This should be done from a shell window that
is in this (espd) directory.

Then set up a host patch (examples in the subdirectory host).  This patch
listens on port 4498 (by default).  When a connection os made, it loads a test
patch on the esp, by sending the message "pd begin-new poodle .", then the contents
of the patch, then "pd end-new"

DOLIST
toggles to indicate connectedness
why is first message after restarting card dropped?
move patch-loader out of tcpconnect
sleeping between attempts to connect to host
sleep message to pd object

later:
fold in the network Pd objects?
adapt to non-lyraT boards
