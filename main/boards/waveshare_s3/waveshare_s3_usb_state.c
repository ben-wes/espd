/*
 * VBUS: GPIO, UPS HAT (E) over I2C, or disabled — see board_profile.h.
 */

#include "waveshare_s3_usb_state.h"

#include "board_profile.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waveshare_usb";

static bool s_vbus_gpio_inited;
static i2c_master_dev_handle_t s_ups_i2c_dev;

bool espd_waveshare_usb_vbus_monitoring_configured(void)
{
#if ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E
    return true;
#elif ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_GPIO
    return ESPD_WAVESHARE_USB_VBUS_GPIO >= 0;
#else
    return false;
#endif
}

#if ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E

static esp_err_t ups_hat_read_charging_byte_ephemeral(uint8_t *out)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = ESPD_WAVESHARE_I2C_SDA_GPIO,
        .scl_io_num = ESPD_WAVESHARE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK)
        return err;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESPD_WAVESHARE_UPS_HAT_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dcfg, &dev);
    if (err != ESP_OK)
        goto del_bus;

    uint8_t reg = ESPD_WAVESHARE_UPS_HAT_REG_CHARGING;
    err = i2c_master_transmit_receive(dev, &reg, 1, out, 1, pdMS_TO_TICKS(80));
    i2c_master_bus_rm_device(dev);
del_bus:
    i2c_del_master_bus(bus);
    return err;
}

static bool ups_hat_vbus_from_byte(uint8_t charging_reg)
{
    return (charging_reg >> ESPD_WAVESHARE_UPS_HAT_VBUS_BIT) & 1;
}

#endif /* UPS HAT E */

void espd_waveshare_usb_vbus_init(void)
{
#if ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_GPIO
    if (ESPD_WAVESHARE_USB_VBUS_GPIO < 0 || s_vbus_gpio_inited)
        return;

    gpio_num_t pin = (gpio_num_t)ESPD_WAVESHARE_USB_VBUS_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH ? GPIO_PULLUP_DISABLE
                                                         : GPIO_PULLUP_ENABLE,
        .pull_down_en = ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH ? GPIO_PULLDOWN_ENABLE
                                                           : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK)
        ESP_LOGE(TAG, "gpio_config failed for VBUS GPIO %d", ESPD_WAVESHARE_USB_VBUS_GPIO);
    else
        ESP_LOGI(TAG, "VBUS sense on GPIO %d (active-%s)", ESPD_WAVESHARE_USB_VBUS_GPIO,
                 ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH ? "high" : "low");
    s_vbus_gpio_inited = true;
#elif ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E
    ESP_LOGI(TAG,
             "VBUS via UPS HAT (E) I2C 0x%02X reg 0x%02X bit %d (same bus as ES8311)",
             ESPD_WAVESHARE_UPS_HAT_I2C_ADDR, ESPD_WAVESHARE_UPS_HAT_REG_CHARGING,
             ESPD_WAVESHARE_UPS_HAT_VBUS_BIT);
#endif
}

void espd_waveshare_usb_state_register_i2c_bus(i2c_master_bus_handle_t bus)
{
#if ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E
    if (s_ups_i2c_dev || bus == NULL)
        return;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESPD_WAVESHARE_UPS_HAT_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &s_ups_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "UPS HAT (E) at 0x%02X not found on I2C (err=%s) — hotplug VBUS may fail after boot",
                 ESPD_WAVESHARE_UPS_HAT_I2C_ADDR, esp_err_to_name(err));
        s_ups_i2c_dev = NULL;
    } else {
        ESP_LOGI(TAG, "UPS HAT (E) I2C device registered for VBUS polling");
    }
#else
    (void)bus;
#endif
}

bool espd_waveshare_usb_vbus_present(void)
{
#if ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_GPIO
    if (ESPD_WAVESHARE_USB_VBUS_GPIO < 0)
        return false;
    if (!s_vbus_gpio_inited)
        espd_waveshare_usb_vbus_init();
    int level = gpio_get_level((gpio_num_t)ESPD_WAVESHARE_USB_VBUS_GPIO);
#if ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH
    return level != 0;
#else
    return level == 0;
#endif

#elif ESPD_WAVESHARE_VBUS_BACKEND == ESPD_WAVESHARE_VBUS_BACKEND_UPS_HAT_E
    uint8_t b = 0;
    esp_err_t err;
    if (s_ups_i2c_dev) {
        uint8_t reg = ESPD_WAVESHARE_UPS_HAT_REG_CHARGING;
        err = i2c_master_transmit_receive(s_ups_i2c_dev, &reg, 1, &b, 1,
            pdMS_TO_TICKS(40));
    } else {
        err = ups_hat_read_charging_byte_ephemeral(&b);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "UPS HAT read reg 0x%02X failed: %s",
                 ESPD_WAVESHARE_UPS_HAT_REG_CHARGING, esp_err_to_name(err));
        return false;
    }
    return ups_hat_vbus_from_byte(b);
#else
    return false;
#endif
}

void espd_waveshare_s3_run_disc_mode_until_unplug(void)
{
    ESP_LOGW(TAG,
             "USB host power detected — USB MSC disk mode is not implemented yet. "
             "Remove Type-C / VBUS to start audio.");
    while (espd_waveshare_usb_vbus_present())
        vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "VBUS released — continuing with audio startup.");
}

void espd_waveshare_s3_poll_usb_hotplug_restart(void)
{
    if (!espd_waveshare_usb_vbus_monitoring_configured())
        return;

    static int candidate = -1;
    static int count;
    static bool have_stable;
    static int stable_val;

    int v = espd_waveshare_usb_vbus_present() ? 1 : 0;
    if (v != candidate) {
        candidate = v;
        count = 1;
        return;
    }
    if (++count < 25)
        return;

    count = 0;
    if (!have_stable) {
        have_stable = true;
        stable_val = v;
        return;
    }
    if (v != stable_val) {
        ESP_LOGI(TAG, "VBUS changed (%d -> %d) — restarting for USB/audio mode switch",
                 stable_val, v);
        esp_restart();
    }
}
