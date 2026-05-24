if(CONFIG_ESPD_BOARD_WAVESHARE_S3)
    idf_component_register(
        SRCS "waveshare_io.c" "waveshare_audio.c"
    )
else()
    idf_component_register()
endif()
