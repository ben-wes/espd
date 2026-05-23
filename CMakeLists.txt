# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# for LyraT boards uncomment this line and use "esp_adf" instead of "esp_idf":
#
# include($ENV{ADF_PATH}/CMakeLists.txt)
#

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

option(ESPD_BSP_COMPONENT "Override CONFIG_ESPD_BSP_COMPONENT_NAME at configure time" "")

# Merge one board profile defaults file: generic I2S or the active BSP package.
# BSP boards set CONFIG_ESPD_BSP_COMPONENT_NAME in their Kconfig; generic leaves
# it empty. Never merge both profiles.
set(_espd_profile_defaults "${CMAKE_SOURCE_DIR}/main/boards/generic/sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(READ "${CMAKE_SOURCE_DIR}/sdkconfig" _espd_sdkconfig)
    if(_espd_sdkconfig MATCHES "CONFIG_ESPD_BSP_COMPONENT_NAME=\"([^\"]+)\"")
        set(_espd_bsp_name "${CMAKE_MATCH_1}")
        set(_espd_bsp_defaults
            "${CMAKE_SOURCE_DIR}/components/${_espd_bsp_name}/sdkconfig.defaults")
        if(EXISTS "${_espd_bsp_defaults}")
            set(_espd_profile_defaults "${_espd_bsp_defaults}")
        endif()
    endif()
endif()
list(APPEND SDKCONFIG_DEFAULTS "${_espd_profile_defaults}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espd)
