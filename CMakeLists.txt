# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# for LyraT boards uncomment this line and use "esp_adf" instead of "esp_idf":
#
# include($ENV{ADF_PATH}/CMakeLists.txt)
#

# Generic ESPD profile (always merged).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_SOURCE_DIR}/main/boards/generic/sdkconfig.defaults")

option(ESPD_BSP_COMPONENT "Override CONFIG_ESPD_BSP_COMPONENT_NAME at configure time" "")

# After menuconfig selects a board, merge that BSP's sdkconfig.defaults (if present).
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(READ "${CMAKE_SOURCE_DIR}/sdkconfig" _espd_sdkconfig)
    if(_espd_sdkconfig MATCHES "CONFIG_ESPD_BSP_COMPONENT_NAME=\"([^\"]*)\"")
        set(_espd_bsp_defaults "${CMAKE_SOURCE_DIR}/components/${CMAKE_MATCH_1}/sdkconfig.defaults")
        if(EXISTS "${_espd_bsp_defaults}")
            list(APPEND SDKCONFIG_DEFAULTS "${_espd_bsp_defaults}")
        endif()
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espd)
