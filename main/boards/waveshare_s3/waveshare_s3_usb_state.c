/*
 * See boards/waveshare_s3/README.txt — VBUS GPIO is optional until schematic pin is known.
 */

#include "waveshare_s3_usb_state.h"

#include "board_profile.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waveshare_usb";

static bool s_vbus_gpio_inited;

bool espd_waveshare_usb_vbus_gpio_configured(void)
{
    return ESPD_WAVESHARE_USB_VBUS_GPIO >= 0;
}

void espd_waveshare_usb_vbus_init(void)
{
    if (!espd_waveshare_usb_vbus_gpio_configured() || s_vbus_gpio_inited)
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
}

bool espd_waveshare_usb_vbus_present(void)
{
    if (!espd_waveshare_usb_vbus_gpio_configured())
        return false;
    if (!s_vbus_gpio_inited)
        espd_waveshare_usb_vbus_init();
    int level = gpio_get_level((gpio_num_t)ESPD_WAVESHARE_USB_VBUS_GPIO);
#if ESPD_WAVESHARE_USB_VBUS_ACTIVE_HIGH
    return level != 0;
#else
    return level == 0;
#endif
}

void espd_waveshare_s3_run_disc_mode_until_unplug(void)
{
    ESP_LOGW(TAG,
             "USB host power detected — USB MSC disk mode is not implemented yet. "
             "Unplug USB (or clear VBUS GPIO) to start audio.");
    while (espd_waveshare_usb_vbus_present())
        vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "VBUS released — continuing with audio startup.");
}

void espd_waveshare_s3_poll_usb_hotplug_restart(void)
{
    if (!espd_waveshare_usb_vbus_gpio_configured())
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
