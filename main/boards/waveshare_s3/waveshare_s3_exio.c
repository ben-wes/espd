#include "boards/waveshare_s3/board_profile.h"
#include "boards/waveshare_s3/waveshare_s3_exio.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "waveshare_exio";

#define TCA9554_REG_INPUT 0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_POLARITY 0x02
#define TCA9554_REG_CONFIG 0x03

#define EXIO_PIN_CAMSEL 6
#define EXIO_PIN_USBSEL 7

static esp_err_t tca9554_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
    uint8_t *out)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, pdMS_TO_TICKS(80));
}

static esp_err_t tca9554_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
    uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(80));
}

static esp_err_t apply_on_dev(i2c_master_dev_handle_t dev)
{
    uint8_t cfg = 0;
    uint8_t out = 0;
    esp_err_t err = tca9554_read_reg(dev, TCA9554_REG_CONFIG, &cfg);
    if (err != ESP_OK)
        return err;

    cfg &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    err = tca9554_write_reg(dev, TCA9554_REG_CONFIG, cfg);
    if (err != ESP_OK)
        return err;

    err = tca9554_read_reg(dev, TCA9554_REG_OUTPUT, &out);
    if (err != ESP_OK)
        return err;

    out &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_USBSEL);
    if (ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_CAMSEL);

    err = tca9554_write_reg(dev, TCA9554_REG_OUTPUT, out);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
            "TCA9554 @0x%02X: EXIO7(usb route)=%d EXIO6(cam_sel)=%d",
            ESPD_WAVESHARE_TCA9554_I2C_ADDR, ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL,
            ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL);
    }
    (void)TCA9554_REG_INPUT;
    (void)TCA9554_REG_POLARITY;
    return err;
}

esp_err_t espd_waveshare_exio_apply_usb_mux(i2c_master_bus_handle_t bus)
{
    if (!bus)
        return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESPD_WAVESHARE_TCA9554_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &dev);
    if (err != ESP_OK)
        return err;

    err = apply_on_dev(dev);
    i2c_master_bus_rm_device(dev);
    return err;
}

esp_err_t espd_waveshare_exio_apply_usb_mux_ephemeral(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = ESPD_WAVESHARE_I2C_SDA_GPIO,
        .scl_io_num = ESPD_WAVESHARE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK)
        return err;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESPD_WAVESHARE_TCA9554_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dcfg, &dev);
    if (err != ESP_OK)
        goto del_bus;

    err = apply_on_dev(dev);
    i2c_master_bus_rm_device(dev);
del_bus:
    i2c_del_master_bus(bus);
    return err;
}
