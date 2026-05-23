/*
 * Waveshare ESP32-S3-AUDIO-Board BSP: I2C, TCA9555 I/O expander, I2S, ES8311/ES7210.
 */

#include "bsp/waveshare_s3.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"
#include "esp_codec_dev_vol.h"
#include "espd_runtime_config.h"
#include "esp_log.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bsp_waveshare_s3";

#define I2C_TIMEOUT_MS 200
#define MCLK_MULTIPLE 256

/* TCA9555 (16-bit) register map */
#define TCA9555_REG_INPUT0  0x00
#define TCA9555_REG_OUTPUT0 0x02
#define TCA9555_REG_CONFIG0 0x06
#define TCA9555_REG_CONFIG1 0x07
#define TCA9555_REG_OUTPUT1 0x03

/* TCA9554 (8-bit) register map */
#define TCA9554_REG_INPUT  0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

#define EXIO_PIN_SD_CS   3
#define EXIO_PIN_CAMSEL  6
#define EXIO_PIN_USBSEL  7

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_codec_dev_handle_t s_codec_dac;
#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
static esp_codec_dev_handle_t s_codec_mic;
#endif

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
    const uint8_t pa_mask = BSP_WAVESHARE_PA_PORT1_MASK;

    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg0), TAG, "cfg0");
    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_CONFIG1, &cfg1), TAG, "cfg1");

    cfg0 &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (pa_mask)
        cfg1 &= (uint8_t)~pa_mask;

    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_CONFIG0, cfg0), TAG, "w cfg0");
    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_CONFIG1, cfg1), TAG, "w cfg1");

    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_OUTPUT0, &out0), TAG, "out0");
    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_OUTPUT1, &out1), TAG, "out1");

    out0 &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL)
        out0 |= (uint8_t)(1U << EXIO_PIN_USBSEL);
    if (BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL)
        out0 |= (uint8_t)(1U << EXIO_PIN_CAMSEL);

    if (pa_mask) {
        out1 &= (uint8_t)~pa_mask;
        if (BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH)
            out1 |= pa_mask;
    }

    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_OUTPUT0, out0), TAG, "w out0");
    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_OUTPUT1, out1), TAG, "w out1");

    ESP_LOGI(TAG,
        "TCA9555 @0x%02X: usb_exio7=%d cam_exio6=%d pa_port1_mask=0x%02x active_high=%d",
        i2c_7bit, BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL,
        BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL, pa_mask,
        BSP_WAVESHARE_PA_PORT1_ACTIVE_HIGH);
    return ESP_OK;
}

static esp_err_t apply_tca9554(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg, out;

    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg), TAG, "cfg");
    cfg &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9554_REG_CONFIG, cfg), TAG, "w cfg");

    ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9554_REG_OUTPUT, &out), TAG, "out");
    out &= (uint8_t)~((1U << EXIO_PIN_CAMSEL) | (1U << EXIO_PIN_USBSEL));
    if (BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_USBSEL);
    if (BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL)
        out |= (uint8_t)(1U << EXIO_PIN_CAMSEL);

    ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9554_REG_OUTPUT, out), TAG, "w out");

    if (BSP_WAVESHARE_PA_PORT1_MASK != 0)
        ESP_LOGW(TAG,
            "TCA9554 @0x%02X: USB mux OK; PA mask 0x%02x ignored (no port1)",
            i2c_7bit, (unsigned)BSP_WAVESHARE_PA_PORT1_MASK);

    ESP_LOGI(TAG, "TCA9554 @0x%02X: usb_exio7=%d cam_exio6=%d", i2c_7bit,
        BSP_WAVESHARE_EXIO7_USB_ROUTE_LEVEL, BSP_WAVESHARE_EXIO6_CAMERA_SEL_LEVEL);
    return ESP_OK;
}

static esp_err_t apply_on_dev(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t in0;
    esp_err_t err = ioexp_read_reg(dev, TCA9554_REG_INPUT, &in0);
    if (err != ESP_OK)
        return err;

    err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &in0);
    if (err == ESP_OK)
        return apply_tca9555(dev, i2c_7bit);

    err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &in0);
    if (err == ESP_OK)
        return apply_tca9554(dev, i2c_7bit);

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
    push_unique(out, &n, max_n, BSP_IO_EXPANDER_I2C_ADDR);
    push_unique(out, &n, max_n, BSP_IO_EXPANDER_I2C_ADDR_ALT);
    for (int a = 0x20; a <= 0x27; a++)
        push_unique(out, &n, max_n, (uint8_t)a);
    *out_n = n;
}

static esp_err_t mux_device_apply(i2c_master_bus_handle_t bus)
{
    uint8_t addrs[16];
    unsigned n = 0;
    esp_err_t last = ESP_ERR_NOT_FOUND;

    build_addr_list(addrs, &n, sizeof addrs);

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

    ESP_LOGE(TAG, "No TCA9554/9555 expander responded (last=%s)",
        esp_err_to_name(last));
    return last;
}

static esp_err_t apply_sd_cs_on_dev(i2c_master_dev_handle_t dev, uint8_t i2c_7bit)
{
    uint8_t cfg9555;
    esp_err_t err = ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg9555);
    if (err == ESP_OK) {
        uint8_t cfg0, out0;
        ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_CONFIG0, &cfg0), TAG, "cfg0");
        cfg0 &= (uint8_t)~(1U << EXIO_PIN_SD_CS);
        ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_CONFIG0, cfg0), TAG, "w cfg0");
        ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9555_REG_OUTPUT0, &out0), TAG, "out0");
        out0 |= (uint8_t)(1U << EXIO_PIN_SD_CS);
        ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9555_REG_OUTPUT0, out0), TAG, "w out0");
        ESP_LOGI(TAG, "TCA9555 @0x%02X: SD D3/CS (EXIO3) high", i2c_7bit);
        return ESP_OK;
    }

    uint8_t cfg9554;
    err = ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg9554);
    if (err == ESP_OK) {
        uint8_t cfg, out;
        ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9554_REG_CONFIG, &cfg), TAG, "cfg");
        cfg &= (uint8_t)~(1U << EXIO_PIN_SD_CS);
        ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9554_REG_CONFIG, cfg), TAG, "w cfg");
        ESP_RETURN_ON_ERROR(ioexp_read_reg(dev, TCA9554_REG_OUTPUT, &out), TAG, "out");
        out |= (uint8_t)(1U << EXIO_PIN_SD_CS);
        ESP_RETURN_ON_ERROR(ioexp_write_reg(dev, TCA9554_REG_OUTPUT, out), TAG, "w out");
        ESP_LOGI(TAG, "TCA9554 @0x%02X: SD D3/CS (EXIO3) high", i2c_7bit);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t sd_cs_apply(i2c_master_bus_handle_t bus)
{
    uint8_t addrs[16];
    unsigned n = 0;
    esp_err_t last = ESP_ERR_NOT_FOUND;

    build_addr_list(addrs, &n, sizeof addrs);

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

    ESP_LOGW(TAG, "SD CS/EXIO3: no expander responded");
    return last;
}

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus)
        return ESP_OK;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c_bus;
}

esp_err_t bsp_waveshare_io_expander_apply(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK)
        return err;
    return mux_device_apply(s_i2c_bus);
}

esp_err_t bsp_waveshare_io_expander_sd_cs_high(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK)
        return err;
    return sd_cs_apply(s_i2c_bus);
}

static esp_err_t waveshare_codecs_init(void)
{
    const audio_codec_ctrl_if_t *ctrl8311;
    const audio_codec_data_if_t *data_if;
    const audio_codec_if_t *es8311_if;
    const audio_codec_gpio_if_t *gpio_if;
    es8311_codec_cfg_t es8311_cfg;
    esp_codec_dev_cfg_t dev_cfg = {0};
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = BSP_AUDIO_SAMPLE_RATE_HZ,
    };

    audio_codec_i2c_cfg_t i2c8311 = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    ctrl8311 = audio_codec_new_i2c_ctrl(&i2c8311);
    ESP_RETURN_ON_FALSE(ctrl8311, ESP_FAIL, TAG, "audio_codec_new_i2c_ctrl ES8311");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio_codec_new_i2s_data");

    gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio_codec_new_gpio");

    es8311_cfg = (es8311_codec_cfg_t){
        .ctrl_if = ctrl8311,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = -1,
        .pa_reverted = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
        .mclk_div = MCLK_MULTIPLE,
    };
    es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "es8311_codec_new");

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es8311_if;
    dev_cfg.data_if = data_if;
    s_codec_dac = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_dac, ESP_FAIL, TAG, "esp_codec_dev_new dac");

    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec_dac, &sample_cfg), TAG,
        "esp_codec_dev_open dac");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec_dac, 100), TAG,
        "esp_codec_dev_set_out_vol");

#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
    audio_codec_i2c_cfg_t i2c7210 = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    {
        uint8_t probe_addr = (uint8_t)ES7210_CODEC_DEFAULT_ADDR;
        if (probe_addr > 0x7f)
            probe_addr = (uint8_t)(probe_addr >> 1);
        if (i2c_master_probe(s_i2c_bus, probe_addr, 50) != ESP_OK)
            ESP_LOGW(TAG, "ES7210 probe miss at 0x%02x; trying init anyway",
                     probe_addr);
    }
    const audio_codec_ctrl_if_t *ctrl7210 = audio_codec_new_i2c_ctrl(&i2c7210);
    if (!ctrl7210) {
        ESP_LOGW(TAG, "ES7210: no I2C ctrl (mic disabled)");
        s_codec_mic = NULL;
    } else {
        es7210_codec_cfg_t es7210_cfg = {
            .ctrl_if = ctrl7210,
            .master_mode = false,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
            .mclk_src = ES7210_MCLK_FROM_PAD,
            .mclk_div = MCLK_MULTIPLE,
        };
        const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
        if (!es7210_if) {
            ESP_LOGW(TAG, "ES7210: codec_new failed");
            s_codec_mic = NULL;
        } else {
            esp_codec_dev_cfg_t mic_cfg = {
                .dev_type = ESP_CODEC_DEV_TYPE_IN,
                .codec_if = es7210_if,
                .data_if = data_if,
            };
            s_codec_mic = esp_codec_dev_new(&mic_cfg);
            if (!s_codec_mic) {
                ESP_LOGW(TAG, "ES7210: esp_codec_dev_new failed");
            } else if (esp_codec_dev_open(s_codec_mic, &sample_cfg) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "ES7210: esp_codec_dev_open failed");
                s_codec_mic = NULL;
            } else if (esp_codec_dev_set_in_gain(s_codec_mic, 30.0f) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "ES7210: set_in_gain failed");
            } else {
                ESP_LOGI(TAG, "ES7210 mic capture enabled (stereo)");
            }
        }
    }
#endif
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    if (s_codec_dac)
        return ESP_OK;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");

    if (bsp_waveshare_io_expander_apply() != ESP_OK)
        ESP_LOGW(TAG, "TCA9555 EXIO mux / PA failed (USB or speaker may not work)");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = (uint32_t)espd_audio_dma_desc_num();
    chan_cfg.dma_frame_num = (uint32_t)espd_audio_dma_frame_num();

#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG,
        "i2s_new_channel");
#else
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG,
        "i2s_new_channel");
#endif

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG,
        "i2s tx init");
#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG,
        "i2s rx init");
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s tx enable");
#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "i2s rx enable");
#endif

    ESP_RETURN_ON_ERROR(waveshare_codecs_init(), TAG, "codecs init");

    if (bsp_waveshare_io_expander_apply() != ESP_OK)
        ESP_LOGW(TAG, "TCA9555 re-apply after codec open failed");

    ESP_LOGI(TAG, "audio ready (%d Hz, ES8311%s)", BSP_AUDIO_SAMPLE_RATE_HZ,
#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
        " + ES7210");
#else
        ")");
#endif
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return s_codec_dac;
}

#if BSP_CAPS_AUDIO_MIC
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
#if CONFIG_BSP_WAVESHARE_ENABLE_MIC
    return s_codec_mic;
#else
    return NULL;
#endif
}
#endif
