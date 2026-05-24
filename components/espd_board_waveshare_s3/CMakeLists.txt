# Auto-generated from boards/waveshare_s3.yaml — do not edit.

if(CONFIG_ESPD_BOARD_ESP_BSP_GLUE)
    set(_espd_glue "${CMAKE_CURRENT_LIST_DIR}/../espd_integration")
    idf_component_register(
        SRCS
            "${_espd_glue}/espd_bsp_esp_bsp_audio.c"
            "${_espd_glue}/espd_bsp_esp_bsp_io.c"
        INCLUDE_DIRS "."
    )
else()
    idf_component_register()
endif()
