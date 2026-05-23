#include "boards/waveshare_s3/espd_board.h"
#include "boards/waveshare_s3/waveshare_s3_buttons.h"
#include "bsp/waveshare_s3.h"
#include "espd.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "waveshare_buttons";

#define I2C_TIMEOUT_MS 200

#ifndef ESPD_BUTTON_POLL_TASK_PRIO
#define ESPD_BUTTON_POLL_TASK_PRIO 2  /* well below audio task */
#endif
#ifndef ESPD_BUTTON_POLL_TASK_CORE
#define ESPD_BUTTON_POLL_TASK_CORE 0  /* opposite core from Pd audio (core 1) */
#endif
#ifndef ESPD_BUTTON_POLL_PERIOD_MS
#define ESPD_BUTTON_POLL_PERIOD_MS 5  /* poll every 5ms = 200 Hz */
#endif

/* TCA9555 register map (port1 holds EXIO8..15, so EXIO9/10/11 = bits 1/2/3). */
#define TCA9555_REG_INPUT1 0x01

/* Bits on port1 that the three buttons occupy. See espd_board.h. */
#ifndef ESPD_WAVESHARE_BUTTON0_PORT1_BIT
#define ESPD_WAVESHARE_BUTTON0_PORT1_BIT 1  /* EXIO9 / P11 */
#endif
#ifndef ESPD_WAVESHARE_BUTTON1_PORT1_BIT
#define ESPD_WAVESHARE_BUTTON1_PORT1_BIT 2  /* EXIO10 / P12 */
#endif
#ifndef ESPD_WAVESHARE_BUTTON2_PORT1_BIT
#define ESPD_WAVESHARE_BUTTON2_PORT1_BIT 3  /* EXIO11 / P13 */
#endif

    static const int bits[3] = {
        ESPD_WAVESHARE_BUTTON0_PORT1_BIT,
        ESPD_WAVESHARE_BUTTON1_PORT1_BIT,
        ESPD_WAVESHARE_BUTTON2_PORT1_BIT,
    };
    /* Debounce: only accept a new state after two consecutive matching polls. */
    static int last_stable[3] = {0, 0, 0};
    static int last_raw[3] = {0, 0, 0};
    static int last_reported[3] = {0, 0, 0}; 
    static int initialized;

/*
 * Implementation note (IDF v6 i2c_master quirk observed on this board):
 * Repeated add_device + transmit_receive + rm_device cycles on the shared bus
 * eventually NACK at the same address that worked seconds before — even with
 * no other persistent devices attached. exio.c only avoids this because it
 * succeeds on its first probe attempt. To stay robust we add the TCA9555
 * device once at init and KEEP the handle for all subsequent reads.
 */
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_poll_task;
/* Dirty flag: set by button task when state changes, cleared by audio thread. */
static volatile int s_button_dirty;

static void espd_buttons_poll_task(void *arg);

esp_err_t espd_waveshare_s3_buttons_init(void)
{
    if (s_dev)
        return ESP_OK;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "shared I2C bus not ready; buttons disabled");
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESPD_WAVESHARE_TCA9555_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_device @0x%02X failed: %s",
            (unsigned)ESPD_WAVESHARE_TCA9555_I2C_ADDR, esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }
    /* Sanity-check the chip is actually there by reading INPUT0. */
    uint8_t reg = 0x00, in0 = 0;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &in0, 1,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 @0x%02X read INPUT0 failed: %s; buttons disabled",
            (unsigned)ESPD_WAVESHARE_TCA9555_I2C_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }

    ESP_LOGI(TAG, "TCA9555 @0x%02X ready: INPUT0=0x%02x; buttons on port1 bits "
        "%d/%d/%d (EXIO%d/%d/%d)",
        (unsigned)ESPD_WAVESHARE_TCA9555_I2C_ADDR, in0,
        ESPD_WAVESHARE_BUTTON0_PORT1_BIT,
        ESPD_WAVESHARE_BUTTON1_PORT1_BIT,
        ESPD_WAVESHARE_BUTTON2_PORT1_BIT,
        8 + ESPD_WAVESHARE_BUTTON0_PORT1_BIT,
        8 + ESPD_WAVESHARE_BUTTON1_PORT1_BIT,
        8 + ESPD_WAVESHARE_BUTTON2_PORT1_BIT);

    /* Spawn the dedicated poll task so I2C reads never run on the audio
     * thread. Pinned to the opposite core to avoid sharing CPU with senddacs(). */
    if (!s_poll_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_buttons_poll_task,
            "button_poll", 2048, NULL, ESPD_BUTTON_POLL_TASK_PRIO,
            &s_poll_task, ESPD_BUTTON_POLL_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create button_poll task; falling back to audio-thread polling");
            s_poll_task = NULL;
        } else {
            ESP_LOGI(TAG, "button_poll task: core=%d prio=%d period=%d ms",
                (int)ESPD_BUTTON_POLL_TASK_CORE, (int)ESPD_BUTTON_POLL_TASK_PRIO,
                ESPD_BUTTON_POLL_PERIOD_MS);
        }
    }
    return ESP_OK;
}

/* Weak default so boards/builds without Pd linkage still compile; espd.c overrides. */
__attribute__((weak)) void espd_button_state_changed(int idx, int pressed)
{
    (void)idx;
    (void)pressed;
}

static void espd_buttons_poll_task(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    uint8_t reg = TCA9555_REG_INPUT1, in1 = 0;
    int i;

    for (;;) {
        TickType_t period = pdMS_TO_TICKS(ESPD_BUTTON_POLL_PERIOD_MS);
        if (period < 1)
            period = 1;
        vTaskDelayUntil(&next, period);

        if (i2c_master_transmit_receive(s_dev, &reg, 1, &in1, 1,
                pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != ESP_OK)
            continue;

        for (i = 0; i < 3; i++) {
            /* Buttons pull to GND when pressed; invert so "pressed" == 1. */
            int raw = ((in1 >> bits[i]) & 1) ? 0 : 1;
            if (raw == last_raw[i] && raw != last_stable[i]) {
                last_stable[i] = raw;
                s_button_dirty = 1;
            }
            last_raw[i] = raw;
        }
    }
}
/* Consumer: audio thread. Poll dirty flag and forward button states to Pd. */
void espd_waveshare_s3_buttons_poll(void)
{
    if (!s_button_dirty)
        return;

    s_button_dirty = 0;
    for (int i = 0; i < 3; i++) {
        if (last_stable[i] != last_reported[i]) {
            last_reported[i] = last_stable[i];
            espd_button_state_changed(i, last_stable[i]);
        }
    }
}
