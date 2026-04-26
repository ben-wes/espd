list(APPEND MAIN_SRCS
    "boards/waveshare_s3/waveshare_s3_audio.c"
    "boards/waveshare_s3/waveshare_s3_buttons.c"
    "boards/waveshare_s3/waveshare_s3_exio.c"
    "boards/waveshare_s3/waveshare_s3_leds.c"
    "boards/waveshare_s3/waveshare_s3_sdcard.c")

list(APPEND MAIN_REQUIRES fatfs esp_driver_sdmmc)
if(CONFIG_TINYUSB_CDC_ENABLED)
    list(APPEND MAIN_REQUIRES esp_tinyusb usb)
endif()

list(APPEND MAIN_BOARD_DEFS ESPD_BOARD_WAVESHARE_S3=1)
