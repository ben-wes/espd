# Links enabled espd_board_* plugins (auto-discovered by naming convention).
# Plugin espd_board_foo_bar is enabled when CONFIG_ESPD_BOARD_FOO_BAR=y.
# Kconfig.inc is generated in root CMakeLists.txt before project().

set(_espd_board_reqs espd_integration)

# GLOB needs an absolute pattern; the RELATIVE base is only used to trim the
# returned paths, NOT to anchor the pattern itself.
file(GLOB _espd_board_plugins RELATIVE "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../espd_board_*")

foreach(_plugin IN LISTS _espd_board_plugins)
    string(REGEX REPLACE "^espd_board_" "" _suffix "${_plugin}")
    string(TOUPPER "${_suffix}" _suffix_upper)
    set(_config_sym "CONFIG_ESPD_BOARD_${_suffix_upper}")
    if(${_config_sym})
        list(APPEND _espd_board_reqs "${_plugin}")
    endif()
endforeach()

idf_component_register(REQUIRES ${_espd_board_reqs})
