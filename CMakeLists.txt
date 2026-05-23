# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# for LyraT boards uncomment this line and use "esp_adf" instead of "esp_idf":
#
# include($ENV{ADF_PATH}/CMakeLists.txt)
#

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

# Merge at most one board profile defaults file. Never default to generic — that
# profile is for CONFIG_ESPD_BOARD_GENERIC only and must not run before a BSP
# board is chosen (would stick 32768 stack / legacy WiFi on esp32s3 builds).
set(_espd_profile_defaults "")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(READ "${CMAKE_SOURCE_DIR}/sdkconfig" _espd_sdkconfig)

    if(_espd_sdkconfig MATCHES "CONFIG_ESPD_BOARD_GENERIC=y")
        set(_espd_profile_defaults
            "${CMAKE_SOURCE_DIR}/main/boards/generic/sdkconfig.defaults")
    elseif(_espd_sdkconfig MATCHES "CONFIG_ESPD_BOARD_WAVESHARE_S3=y")
        set(_espd_profile_defaults
            "${CMAKE_SOURCE_DIR}/components/espd_bsp_selector/sdkconfig.defaults")
    else()
        string(REGEX MATCHALL "CONFIG_ESPD_BOARD_[A-Z0-9_]+=y" _espd_board_syms
            "${_espd_sdkconfig}")
        foreach(_sym IN LISTS _espd_board_syms)
            if(_sym STREQUAL "CONFIG_ESPD_BOARD_GENERIC=y")
                continue()
            endif()
            if(_sym MATCHES "CONFIG_ESPD_BOARD_([A-Z0-9_]+)=y")
                string(TOLOWER "${CMAKE_MATCH_1}" _board_suffix)
                set(_candidate
                    "${CMAKE_SOURCE_DIR}/components/espd_bsp_selector/sdkconfig.defaults")
                if(EXISTS "${_candidate}")
                    set(_espd_profile_defaults "${_candidate}")
                    break()
                endif()
            endif()
        endforeach()
    endif()
endif()

if(_espd_profile_defaults)
    list(APPEND SDKCONFIG_DEFAULTS "${_espd_profile_defaults}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espd)
