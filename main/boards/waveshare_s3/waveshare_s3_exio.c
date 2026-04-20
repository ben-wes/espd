#include "boards/waveshare_s3/board_profile.h"
#include "boards/waveshare_s3/waveshare_s3_exio.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "waveshare_exio";

#define I2C_TIMEOUT_MS 200

/* TCA9555 (16-bit): TI register map */
#define TCA9555_REG_INPUT0 0x00
#define TCA9555_REG_INPUT1 0x01
#define TCA9555_REG_OUTPUT0 0x02
#define TCA9555_REG_OUTPUT1 0x03
#define TCA9555_REG_CONFIG0 0x06
#define TCA9555_REG_CONFIG1 0x07

/* TCA9554 (8-bit): only regs 0x00–0x03; reading 0x06 NACKs — old code looked like 9554 at 0x22 */
#define TCA9554_REG_INPUT 0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

#define EXIO_PIN_CAMSEL 6
#define EXIO_PIN_USBSEL 7
/** Wiki: SD_D3 / CS -> EXIO3 (TCA9555 port0 bit 3). */
#define EXIO_PIN_SD_CS 3

static esp_err_t ioexp_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
    uint8_t *out)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t ioexp_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
    uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t apply_tca9555(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg0, cfg1, out0, out1;
    esp_err_t err;
    const uint8_t pa_mask = (uint8_t)ESPD_WAVESHARE_TCA9555_PA_PORT1_MASK;

    err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg0);
    if (err != ESP_OK)
        return err;
    err = ioexp_read_reg(dev, TCA9555_REG_CONFIG1, &cfg1);
    if (err != ESP_OK)
        return err;

    cfg0 &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (pa_mask)
        cfg1 &= (uint8_t)~pa_mask;

    err = ioexp_write_reg(dev, TCA9555_REG_CONFIG0, cfg0);
    if (err != ESP_OK)
        return err;
    err = ioexp_write_reg(dev, TCA9555_REG_CONFIG1, cfg1);
    if (err != ESP_OK)
        return err;

    err = ioexp_read_reg(dev, TCA9555_REG_OUTPUT0, &out0);
    if (err != ESP_OK)
        return err;
    err = ioexp_read_reg(dev, TCA9555_REG_OUTPUT1, &out1);
    if (err != ESP_OK)
        return err;

    out0 &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL)
        out0 |= (uint8_t)(1U << EXIO_PIN_USBSEL);
    if (ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL)
        out0 |= (uint8_t)(1U << EXIO_PIN_CAMSEL);

    if (pa_mask) {
        out1 &= (uint8_t)~pa_mask;
        if (ESPD_WAVESHARE_PA_PORT1_ACTIVE_HIGH)
            out1 |= pa_mask;
    }

    err = ioexp_write_reg(dev, TCA9555_REG_OUTPUT0, out0);
    if (err != ESP_OK)
        return err;
    err = ioexp_write_reg(dev, TCA9555_REG_OUTPUT1, out1);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG,
        "TCA9555 @0x%02X: usb_exio7=%d cam_exio6=%d pa_port1_mask=0x%02x active_high=%d",
        i2c_7bit, ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL,
        ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL, pa_mask,
        ESPD_WAVESHARE_PA_PORT1_ACTIVE_HIGH);
    return ESP_OK;
}

static esp_err_t apply_tca9554(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg, out;
    esp_err_t err;

    err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg);
    if (err != ESP_OK)
        return err;

    cfg &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    err = ioexp_write_reg(dev, TCA9554_REG_CONFIG, cfg);
    if (err != ESP_OK)
        return err;

    err = ioexp_read_reg(dev, TCA9554_REG_OUTPUT, &out);
    if (err != ESP_OK)
        return err;

    out &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_USBSEL);
    if (ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_CAMSEL);

    err = ioexp_write_reg(dev, TCA9554_REG_OUTPUT, out);
    if (err != ESP_OK)
        return err;

    if (ESPD_WAVESHARE_TCA9555_PA_PORT1_MASK != 0)
        ESP_LOGW(TAG,
            "TCA9554 @0x%02X: USB mux OK; PA mask 0x%02x ignored (no port1 — use TCA9555 board or direct GPIO)",
            i2c_7bit, (unsigned)ESPD_WAVESHARE_TCA9555_PA_PORT1_MASK);

    ESP_LOGI(TAG, "TCA9554 @0x%02X: usb_exio7=%d cam_exio6=%d", i2c_7bit,
        ESPD_WAVESHARE_EXIO7_USB_ROUTE_LEVEL,
        ESPD_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL);
    return ESP_OK;
}

/*
 * Detect expander: both chips have INPUT at 0x00. TCA9555 has CONFIG0 at 0x06;
 * TCA9554 only has regs 0–3 so 0x06 NACKs — that mismatch caused all probes to fail.
 */
static esp_err_t apply_on_dev(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t in0;
    esp_err_t err = ioexp_read_reg(dev, TCA9554_REG_INPUT, &in0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "probe INPUT@0x%02X: %s", i2c_7bit, esp_err_to_name(err));
        return err;
    }

    uint8_t cfg9555;
    err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg9555);
    if (err == ESP_OK)
        return apply_tca9555(dev, i2c_7bit);

    uint8_t cfg9554;
    err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg9554);
    if (err == ESP_OK)
        return apply_tca9554(dev, i2c_7bit);

    ESP_LOGW(TAG,
        "0x%02X: INPUT ok (0x%02x) but neither CONFIG@0x06 (9555) nor CONFIG@0x03 (9554): %s",
        i2c_7bit, in0, esp_err_to_name(err));
    return err;
}

static void push_unique(uint8_t *out, unsigned *n, unsigned max_n, uint8_t a)
{
    for (unsigned j = 0; j < *n; j++) {
        if (out[j] == a)
            return;
    }
    if (*n < max_n)
        out[(*n)++] = a;
}

static void build_addr_list(uint8_t *out, unsigned *out_n, unsigned max_n)
{
    unsigned n = 0;
    push_unique(out, &n, max_n, ESPD_WAVESHARE_TCA9555_I2C_ADDR);
    push_unique(out, &n, max_n, ESPD_WAVESHARE_TCA9555_I2C_ADDR_ALT);
    for (int a = 0x20; a <= 0x27; a++)
        push_unique(out, &n, max_n, (uint8_t)a);
    *out_n = n;
}

static esp_err_t apply_sd_cs_high_tca9555(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg0, out0;
    esp_err_t err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg0);
    if (err != ESP_OK)
        return err;
    cfg0 &= (uint8_t)~(1U << EXIO_PIN_SD_CS);
    err = ioexp_write_reg(dev, TCA9555_REG_CONFIG0, cfg0);
    if (err != ESP_OK)
        return err;
    err = ioexp_read_reg(dev, TCA9555_REG_OUTPUT0, &out0);
    if (err != ESP_OK)
        return err;
    out0 |= (uint8_t)(1U << EXIO_PIN_SD_CS);
    err = ioexp_write_reg(dev, TCA9555_REG_OUTPUT0, out0);
    if (err != ESP_OK)
        return err;
    ESP_LOGI(TAG, "TCA9555 @0x%02X: SD D3/CS (EXIO3) high", i2c_7bit);
    return ESP_OK;
}

static esp_err_t apply_sd_cs_high_tca9554(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg, out;
    esp_err_t err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg);
    if (err != ESP_OK)
        return err;
    cfg &= (uint8_t)~(1U << EXIO_PIN_SD_CS);
    err = ioexp_write_reg(dev, TCA9554_REG_CONFIG, cfg);
    if (err != ESP_OK)
        return err;
    err = ioexp_read_reg(dev, TCA9554_REG_OUTPUT, &out);
    if (err != ESP_OK)
        return err;
    out |= (uint8_t)(1U << EXIO_PIN_SD_CS);
    err = ioexp_write_reg(dev, TCA9554_REG_OUTPUT, out);
    if (err != ESP_OK)
        return err;
    ESP_LOGI(TAG, "TCA9554 @0x%02X: SD D3/CS (EXIO3) high", i2c_7bit);
    return ESP_OK;
}

static esp_err_t apply_sd_cs_on_dev(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg9555;
    esp_err_t err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg9555);
    if (err == ESP_OK)
        return apply_sd_cs_high_tca9555(dev, i2c_7bit);
    uint8_t cfg9554;
    err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg9554);
    if (err == ESP_OK)
        return apply_sd_cs_high_tca9554(dev, i2c_7bit);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espd_waveshare_exio_sd_cs_high(i2c_master_bus_handle_t bus)
{
    if (!bus)
        return ESP_ERR_INVALID_ARG;

    uint8_t addrs[16];
    unsigned n = 0;
    build_addr_list(addrs, &n, sizeof addrs);
    esp_err_t last = ESP_ERR_NOT_FOUND;

    for (unsigned k = 0; k < n; k++) {
        uint8_t addr = addrs[k];
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &dev);
        if (err != ESP_OK) {
            last = err;
            continue;
        }
        err = apply_sd_cs_on_dev(dev, addr);
        i2c_master_bus_rm_device(dev);
        if (err == ESP_OK)
            return ESP_OK;
        last = err;
    }

    ESP_LOGW(TAG, "SD CS/EXIO3: no TCA9554/9555 responded; SD may still work if D3 is pulled up");
    return last;
}

static esp_err_t mux_device_apply(i2c_master_bus_handle_t bus)
{
    uint8_t addrs[16];
    unsigned n = 0;
    build_addr_list(addrs, &n, sizeof addrs);
    esp_err_t last = ESP_ERR_NOT_FOUND;

    for (unsigned k = 0; k < n; k++) {
        uint8_t addr = addrs[k];
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &dev);
        if (err != ESP_OK) {
            last = err;
            continue;
        }
        err = apply_on_dev(dev, addr);
        i2c_master_bus_rm_device(dev);
        if (err == ESP_OK)
            return ESP_OK;
        last = err;
    }

    ESP_LOGE(TAG, "No TCA9554/TCA9555 expander responded on I2C0 (tried 0x20–0x27); last=%s",
        esp_err_to_name(last));
    return last;
}

esp_err_t espd_waveshare_exio_apply_usb_mux(i2c_master_bus_handle_t bus)
{
    if (!bus)
        return ESP_ERR_INVALID_ARG;

    return mux_device_apply(bus);
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

    err = mux_device_apply(bus);
    i2c_del_master_bus(bus);
    return err;
}
