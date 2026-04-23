set(MAIN_SRCS
    "espd.c"
    "pd_lseek64_compat.c"
    "espd_patch_store.c"
    "pdmain.c"
    "espdsp_osc_override.c"
    "../pd/src/g_canvas.c"
    "../pd/src/g_graph.c"
    "../pd/src/g_text.c"
    "../pd/src/g_rtext.c"
    "../pd/src/g_array.c"
    "../pd/src/g_bang.c"
    "../pd/src/g_template.c"
    "../pd/src/g_io.c"
    "../pd/src/g_scalar.c"
    "../pd/src/g_all_guis.c"
    "../pd/src/g_mycanvas.c"
    "../pd/src/g_numbox.c"
    "../pd/src/g_radio.c"
    "../pd/src/g_guiconnect.c"
    "../pd/src/g_readwrite.c"
    "../pd/src/g_clone.c"
    "../pd/src/g_slider.c"
    "../pd/src/g_toggle.c"
    "../pd/src/g_vumeter.c"
    "../pd/src/m_pd.c"
    "../pd/src/m_class.c"
    "../pd/src/m_obj.c"
    "../pd/src/m_atom.c"
    "../pd/src/m_memory.c"
    "../pd/src/m_binbuf.c"
    "../pd/src/s_net.c"
    "../pd/src/s_print.c"
    "../pd/src/s_utf8.c"
    "../pd/src/d_ugen.c"
    "../pd/src/d_resample.c"
    "../pd/src/d_soundfile.c"
    "../pd/src/d_soundfile_wave.c"
    "../pd/src/d_soundfile_aiff.c"
    "../pd/src/d_soundfile_caf.c"
    "../pd/src/d_soundfile_next.c"
    "../pd/src/d_global.c"
    "../pd/src/d_osc.c"
    "../pd/src/d_ctl.c"
    "../pd/src/d_arithmetic.c"
    "../pd/src/d_array.c"
    "../pd/src/d_dac.c"
    "../pd/src/d_delay.c"
    "../pd/src/d_filter.c"
    "../pd/src/d_math.c"
    "../pd/src/d_misc.c"
    "../pd/src/x_arithmetic.c"
    "../pd/src/x_connective.c"
    "../pd/src/x_interface.c"
    "../pd/src/x_misc.c"
    "../pd/src/x_net.c"
    "../pd/src/x_time.c"
    "../pd/src/x_acoustics.c"
    "../pd/src/x_gui.c"
    "../pd/src/x_array.c"
    "../pd/src/x_scalar.c"
    "../pd/src/x_text.c"
    "../pd/src/x_list.c"
    "../pd/src/x_vexp.c"
    "../pd/src/x_vexp_fun.c"
    "../pd/src/x_vexp_if.c"
    "../pd/src/s_inter_gui.c"
    "../wifi/wifi.c"
    "../wifi/net.c"
)

# FIXME: remove these warning suppression flags once Pd is fixed
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPD -DHAVE_UNISTD_H -DHAVE_ALLOCA_H -DPD_HEADLESS -DPD_INTERNAL -DSYMTABHASHSIZE=512 -DSTUPID_SORT -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-format -Wno-stringop-truncation -Wno-unused-const-variable -Wno-parentheses -Wno-maybe-uninitialized -Wno-unused-label -Wno-char-subscripts -Wno-cast-function-type -Wno-compare-distinct-pointer-types -Wno-cpp -Wno-empty-body -Wno-type-limits -Wno-unterminated-string-initialization -Wno-missing-field-initializers -Wno-implicit-fallthrough")

set(MAIN_REQUIRES
    console
    spiffs
    driver
    esp_adc
    nvs_flash
    esp_timer
    esp_event
    esp_netif
    esp_wifi
    esp_driver_ledc
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    esp_driver_uart
    esp_codec_dev
    lwip
)

set(ESPD_BOARD "$ENV{ESPD_BOARD}")
set(MAIN_BOARD_DEFS "")
if(ESPD_BOARD)
    set(BOARD_CMAKE "${CMAKE_CURRENT_LIST_DIR}/boards/${ESPD_BOARD}/board.cmake")
    if(EXISTS "${BOARD_CMAKE}")
        include("${BOARD_CMAKE}")
    else()
        message(FATAL_ERROR "Unsupported ESPD_BOARD='${ESPD_BOARD}': missing ${BOARD_CMAKE}")
    endif()
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${MAIN_REQUIRES}
)

if(MAIN_BOARD_DEFS)
    idf_component_get_property(main_lib ${COMPONENT_NAME} COMPONENT_LIB)
    target_compile_definitions(${main_lib} PRIVATE ${MAIN_BOARD_DEFS})
endif()

# Keep aggressive math optimization scoped to DSP override kernels only.
set_source_files_properties(
    "espdsp_osc_override.c"
    PROPERTIES COMPILE_OPTIONS "-ffast-math;-fno-math-errno"
)
