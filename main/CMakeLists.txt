set(MAIN_SRCS
    "espd.c"
    "pdmain.c"
    "espdsp_osc_override.c"
    "../pd/src/g_canvas.c"
    "../pd/src/g_graph.c"
    "../pd/src/g_text.c"
    "../pd/src/g_rtext.c"
    "../pd/src/g_array.c"
    "../pd/src/g_template.c"
    "../pd/src/g_io.c"
    "../pd/src/g_scalar.c"
    "../pd/src/g_guiconnect.c"
    "../pd/src/g_readwrite.c"
    "../pd/src/g_clone.c"
    "../pd/src/m_pd.c"
    "../pd/src/m_class.c"
    "../pd/src/m_obj.c"
    "../pd/src/m_atom.c"
    "../pd/src/m_memory.c"
    "../pd/src/m_binbuf.c"
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

if(DEFINED ENV{ESPD_BOARD} AND "$ENV{ESPD_BOARD}" STREQUAL "waveshare_s3")
    list(APPEND MAIN_SRCS "boards/waveshare_s3/waveshare_s3_audio.c")
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -DESPD_BOARD_WAVESHARE_S3=1")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPD -DHAVE_UNISTD_H -DHAVE_ALLOCA_H -DPD_HEADLESS -DPD_INTERNAL -DSYMTABHASHSIZE=512 -DSTUPID_SORT -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-format -Wno-stringop-truncation -Wno-unused-const-variable")

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        driver
        nvs_flash
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        esp_driver_i2s
        esp_driver_i2c
        esp_codec_dev
        lwip
)
