# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16...3.30)

# for LyraT boards uncomment this line and use "esp_adf" instead of "esp_idf":
#
# include($ENV{ADF_PATH}/CMakeLists.txt)
#

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

# Per-chip tuning lives in sdkconfig.defaults.<target> (e.g. sdkconfig.defaults.esp32s3).
# IDF merges "<entry>.<IDF_TARGET>" right after each SDKCONFIG_DEFAULTS entry on its
# own (tools/cmake/kconfig.cmake), so the base file pulls in the chip file with no
# code here — keeping the chip layer IDF tuning only (never a board choice).

# Optional local overlay (menuconfig alternative) — gitignored; kits CI writes this file.
set(_espd_extra_defaults "")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND _espd_extra_defaults "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
endif()
if(DEFINED ENV{ESPD_SDKCONFIG_DEFAULTS} AND NOT "$ENV{ESPD_SDKCONFIG_DEFAULTS}" STREQUAL "")
    string(REPLACE ":" ";" _espd_env_defaults "$ENV{ESPD_SDKCONFIG_DEFAULTS}")
    foreach(_espd_defaults_file IN LISTS _espd_env_defaults)
        if(EXISTS "${_espd_defaults_file}")
            list(APPEND _espd_extra_defaults "${_espd_defaults_file}")
        else()
            message(WARNING "ESPD_SDKCONFIG_DEFAULTS: file not found: ${_espd_defaults_file}")
        endif()
    endforeach()
endif()
foreach(_espd_defaults_file IN LISTS _espd_extra_defaults)
    list(APPEND SDKCONFIG_DEFAULTS "${_espd_defaults_file}")
endforeach()

# Read sdkconfig when present; otherwise scan the defaults that can carry a board
# choice (base + local/env overlays) so the active board is known before the first
# kconfgen run. Chip files (sdkconfig.defaults.<target>) are IDF tuning only and
# never declare a board, so they are not scanned here.
function(_espd_collect_config_text _out_var)
    if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
        file(READ "${CMAKE_SOURCE_DIR}/sdkconfig" _text)
        # set-target may have run before board plugins existed; overlay board choice
        # from defaults.local / ESPD_SDKCONFIG_DEFAULTS when sdkconfig missed it.
        if(NOT _text MATCHES "CONFIG_ESPD_BOARD_[A-Z0-9_]+=y")
            foreach(_defaults_file IN LISTS _espd_extra_defaults)
                if(EXISTS "${_defaults_file}")
                    file(READ "${_defaults_file}" _chunk)
                    string(APPEND _text "${_chunk}\n")
                endif()
            endforeach()
        endif()
    else()
        set(_text "")
        set(_defaults_files "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
        if(_espd_extra_defaults)
            list(APPEND _defaults_files ${_espd_extra_defaults})
        endif()
        foreach(_defaults_file IN LISTS _defaults_files)
            if(EXISTS "${_defaults_file}")
                file(READ "${_defaults_file}" _chunk)
                string(APPEND _text "${_chunk}\n")
            endif()
        endforeach()
    endif()
    set(${_out_var} "${_text}" PARENT_SCOPE)
endfunction()

# Active board suffix (e.g. mykit), or "generic", or "" if unknown.
# Reads the board choice (CONFIG_ESPD_BOARD_*) from sdkconfig or the base/overlay
# defaults; skips choices that do not match the current IDF target.
function(_espd_resolve_active_board _config_text _out_var)
    set(_idf_target "")
    if(DEFINED IDF_TARGET AND NOT "${IDF_TARGET}" STREQUAL "")
        set(_idf_target "${IDF_TARGET}")
    elseif(_config_text MATCHES "CONFIG_IDF_TARGET=\"([^\"]+)\"")
        set(_idf_target "${CMAKE_MATCH_1}")
    elseif(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "")
        set(_idf_target "$ENV{IDF_TARGET}")
    endif()

    set(_suffix "")
    string(REGEX MATCHALL "CONFIG_ESPD_BOARD_[A-Z0-9_]+=y" _espd_board_syms
        "${_config_text}")
    foreach(_sym IN LISTS _espd_board_syms)
        if(_sym STREQUAL "CONFIG_ESPD_BOARD_GENERIC=y")
            continue()
        endif()
        if(_sym MATCHES "CONFIG_ESPD_BOARD_([A-Z0-9_]+)=y")
            string(TOLOWER "${CMAKE_MATCH_1}" _candidate_suffix)
            set(_kconfig_board
                "${CMAKE_SOURCE_DIR}/components/espd_board_${_candidate_suffix}/Kconfig.board")
            if(_idf_target AND EXISTS "${_kconfig_board}")
                file(READ "${_kconfig_board}" _kconfig_text)
                string(TOUPPER "${_idf_target}" _idf_target_upper)
                if(_kconfig_text MATCHES "depends on IDF_TARGET_${_idf_target_upper}")
                    set(_suffix "${_candidate_suffix}")
                    break()
                endif()
            else()
                set(_suffix "${_candidate_suffix}")
                break()
            endif()
        endif()
    endforeach()

    if(_suffix STREQUAL "" AND _config_text MATCHES "CONFIG_ESPD_BOARD_GENERIC=y")
        set(_suffix "generic")
    endif()

    set(${_out_var} "${_suffix}" PARENT_SCOPE)
endfunction()

# Board IDF-tuning profile to merge. BSP boards ship a generated
# components/espd_board_*/sdkconfig.defaults (flash/PHY/console tuning Kconfig
# cannot express). The generic board and the unresolved/default case carry no
# profile: board choice + features are pure Kconfig (choice defaults to generic,
# features follow via Kconfig "imply"), so nothing sticky leaks across boards.
function(_espd_resolve_profile_defaults _config_text _out_var)
    _espd_resolve_active_board("${_config_text}" _active_board)
    set(_profile "")
    if(_active_board AND NOT _active_board STREQUAL "generic")
        set(_profile
            "${CMAKE_SOURCE_DIR}/components/espd_board_${_active_board}/sdkconfig.defaults")
        if(NOT EXISTS "${_profile}")
            set(_profile "")
        endif()
    endif()
    set(${_out_var} "${_profile}" PARENT_SCOPE)
endfunction()

_espd_collect_config_text(_espd_config_text)

# Generate components/espd_board_* from boards/*.yaml before discovery.
if(DEFINED PYTHON AND NOT "${PYTHON}" STREQUAL "")
    set(_espd_python "${PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_espd_python "${Python3_EXECUTABLE}")
endif()
set(_espd_boards_dir "${CMAKE_SOURCE_DIR}/boards")
if(DEFINED ENV{ESPD_BOARDS_DIR} AND NOT "$ENV{ESPD_BOARDS_DIR}" STREQUAL "")
    set(_espd_boards_dir "$ENV{ESPD_BOARDS_DIR}")
endif()
execute_process(
    COMMAND "${_espd_python}" "${CMAKE_SOURCE_DIR}/scripts/gen_board_plugins.py"
            "${CMAKE_SOURCE_DIR}" --boards-dir "${_espd_boards_dir}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _espd_gen_boards_result
    OUTPUT_VARIABLE _espd_gen_boards_out
    ERROR_VARIABLE _espd_gen_boards_err
)
if(NOT _espd_gen_boards_result EQUAL 0)
    message(FATAL_ERROR "gen_board_plugins.py failed: ${_espd_gen_boards_err}")
endif()
if(_espd_gen_boards_out)
    message(STATUS "${_espd_gen_boards_out}")
endif()

# Auto-generate espd_boards/Kconfig.inc before Kconfig runs (board menu entries).
file(GLOB _espd_board_kconfigs "${CMAKE_SOURCE_DIR}/components/espd_board_*/Kconfig.board")
set(_kconfig_inc "# Auto-generated from root CMakeLists.txt — do not edit.\n")
foreach(_board_kconfig IN LISTS _espd_board_kconfigs)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}/components" "${_board_kconfig}")
    string(APPEND _kconfig_inc "orsource \"../${_rel}\"\n")
endforeach()
file(WRITE "${CMAKE_SOURCE_DIR}/components/espd_boards/Kconfig.inc" "${_kconfig_inc}")

# Board plugins (components/espd_board_*) are excluded from the default component
# scan so their idf_component.yml manifests are not resolved until selected in
# menuconfig. Enabled boards are added via EXTRA_COMPONENT_DIRS.
_espd_resolve_active_board("${_espd_config_text}" _espd_active_board)
file(GLOB _espd_board_plugins "${CMAKE_SOURCE_DIR}/components/espd_board_*")
set(_espd_exclude_board_plugins "")
foreach(_plugin_path IN LISTS _espd_board_plugins)
    get_filename_component(_plugin_name "${_plugin_path}" NAME)
    string(REGEX REPLACE "^espd_board_" "" _board_suffix "${_plugin_name}")
    if(_board_suffix STREQUAL _espd_active_board)
        list(APPEND EXTRA_COMPONENT_DIRS "${_plugin_path}")
    else()
        list(APPEND _espd_exclude_board_plugins "${_plugin_name}")
    endif()
endforeach()
if(_espd_exclude_board_plugins)
    set(EXCLUDE_COMPONENTS "${_espd_exclude_board_plugins}")
endif()

# Merge the active BSP board's IDF-tuning profile (sdkconfig or defaults chain).
# Convention: CONFIG_ESPD_BOARD_FOO_BAR=y → components/espd_board_foo_bar/sdkconfig.defaults
# Generic I2S and the default/unresolved case carry no profile (pure Kconfig).

_espd_resolve_profile_defaults("${_espd_config_text}" _espd_profile_defaults)
if(_espd_profile_defaults)
    list(APPEND SDKCONFIG_DEFAULTS "${_espd_profile_defaults}")
endif()

# Add local_components to search path only if the active board requires a local BSP
if(EXISTS "${CMAKE_SOURCE_DIR}/local_components" AND _espd_active_board)
    set(_board_component_yml "${CMAKE_SOURCE_DIR}/components/espd_board_${_espd_active_board}/idf_component.yml")
    if(EXISTS "${_board_component_yml}")
        file(READ "${_board_component_yml}" _yml_content)
        # Extract local component dependency (path: ../../local_components/xxx)
        if(_yml_content MATCHES "[ \t]*path: ../../local_components/([^\n]+)")
            set(_local_bsp_comp "${CMAKE_MATCH_1}")
            set(_local_bsp_path "${CMAKE_SOURCE_DIR}/local_components/${_local_bsp_comp}")
            if(EXISTS "${_local_bsp_path}")
                list(APPEND EXTRA_COMPONENT_DIRS "${_local_bsp_path}")
                message(STATUS "Adding local BSP component: ${_local_bsp_comp}")
            endif()
        else()
        endif()
    endif()
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espd)
