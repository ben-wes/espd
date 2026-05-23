if(CONFIG_ESPD_BOARD_WAVESHARE_S3 STREQUAL "y")
    idf_component_register(
        SRCS "waveshare_io.c"
        INCLUDE_DIRS "include" "."
        REQUIRES espd_bsp_selector espd_integration waveshare_esp32_s3_audio button
    )
else()
    idf_component_register()
endif()
