idf_component_register(
    SRCS "waveshare_s3.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev
)
