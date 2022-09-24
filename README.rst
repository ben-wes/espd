export IDF_TOOLS_PATH=/home/msp/bis/work/esp/toolchain
export ADF_PATH=/home/msp/bis/work/esp/esp-adf
export IDF_PATH=/home/msp/bis/work/esp/esp-idf-v4.4.2
. $ADF_PATH/esp-idf/export.sh

configuring:
enter menuconfig "Component config", choose "Bluetooth"
enter menu Bluetooth, choose "Classic Bluetooth" and "SPP Profile"

idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyUSB0 monitor

docs:
https://gitdemo.readthedocs.io/en/latest/build-system.html
https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html

file:///home/msp/Downloads/WBT101-06A-ClassicBluetooth-Basic-1.pdf
https://people.csail.mit.edu/albert/bluez-intro/c404.html

DOLIST
adapt commector extern to act as TCP server
starting server after ESP fails (can't connect - restart?)
alive message to send count starting w/0
server patch (on host) uploads patch when "alive 0" arrives
message to Pd to restart
restart when recvfrom fails


later:
fold in the network Pd objects?
adapt to non-lyraT boards

