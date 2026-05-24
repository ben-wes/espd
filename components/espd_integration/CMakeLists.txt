set(_espd_integration_srcs
    "bsp_io_stubs.c"
    "espd_runtime_config.c"
)
set(_espd_integration_priv_requires "")

if(CONFIG_ESPD_AUDIO_BACKEND_BSP_CODEC)
    list(APPEND _espd_integration_srcs "espd_bsp_codec_dev.c")
    list(APPEND _espd_integration_priv_requires espressif__esp_codec_dev)
endif()

idf_component_register(
    SRCS ${_espd_integration_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${_espd_integration_priv_requires}
)
