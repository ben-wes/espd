# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# for LyraT boards uncomment this line and use "esp_adf" instead of "esp_idf":
#
# include($ENV{ADF_PATH}/CMakeLists.txt)
#

set(SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_SOURCE_DIR}/components/bsp_waveshare_s3/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espd)
