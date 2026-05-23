idf_component_register(
    SRCS
        "waveshare_s3.c"
        "bsp_led.c"
        "bsp_button.c"
        "bsp_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_codec_dev
        fatfs
        vfs
    PRIV_REQUIRES espressif__led_strip
)
