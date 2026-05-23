idf_component_register(
    SRCS
        "bsp_io_stubs.c"
        "bsp_audio_stubs.c"
        "espd_runtime_config.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_codec_dev
)
