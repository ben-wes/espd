#include "bsp/bsp_button.h"
#include "bsp/waveshare_s3.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_button";

#define I2C_TIMEOUT_MS 200
#define TCA9555_REG_INPUT1 0x01

#ifndef BSP_BUTTON_POLL_TASK_PRIO
#define BSP_BUTTON_POLL_TASK_PRIO 2
#endif
#ifndef BSP_BUTTON_POLL_TASK_CORE
#define BSP_BUTTON_POLL_TASK_CORE 0
#endif
#ifndef BSP_BUTTON_POLL_PERIOD_MS
#define BSP_BUTTON_POLL_PERIOD_MS 5
#endif

static const int s_bits[BSP_BUTTON_COUNT] = {
    BSP_BUTTON0_PORT1_BIT,
    BSP_BUTTON1_PORT1_BIT,
    BSP_BUTTON2_PORT1_BIT,
};

static int s_last_stable[BSP_BUTTON_COUNT];
static int s_last_raw[BSP_BUTTON_COUNT];
static int s_last_reported[BSP_BUTTON_COUNT];
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_poll_task;
static volatile int s_button_dirty;

typedef void (*bsp_button_handler_t)(int idx, int pressed);
static bsp_button_handler_t s_handler;

static void bsp_button_poll_task(void *arg);

int bsp_button_count(void)
{
    return s_dev ? BSP_BUTTON_COUNT : 0;
}

esp_err_t bsp_button_init(void)
{
    esp_err_t err;

    if (s_dev)
        return ESP_OK;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "shared I2C bus not ready; buttons disabled");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_IO_EXPANDER_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_device @0x%02X failed: %s",
            (unsigned)BSP_IO_EXPANDER_I2C_ADDR, esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    {
        uint8_t reg = 0x00, in0 = 0;
        err = i2c_master_transmit_receive(s_dev, &reg, 1, &in0, 1,
            pdMS_TO_TICKS(I2C_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TCA9555 @0x%02X read failed: %s; buttons disabled",
                (unsigned)BSP_IO_EXPANDER_I2C_ADDR, esp_err_to_name(err));
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return err;
        }
        ESP_LOGI(TAG, "TCA9555 @0x%02X ready: INPUT0=0x%02x; buttons on port1 "
            "bits %d/%d/%d",
            (unsigned)BSP_IO_EXPANDER_I2C_ADDR, in0,
            BSP_BUTTON0_PORT1_BIT, BSP_BUTTON1_PORT1_BIT,
            BSP_BUTTON2_PORT1_BIT);
    }

    if (!s_poll_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(bsp_button_poll_task,
            "button_poll", 2048, NULL, BSP_BUTTON_POLL_TASK_PRIO,
            &s_poll_task, BSP_BUTTON_POLL_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create button_poll task");
            s_poll_task = NULL;
        }
    }
    return ESP_OK;
}

void bsp_button_set_handler(void (*handler)(int idx, int pressed))
{
    s_handler = handler;
}

static void bsp_button_poll_task(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    uint8_t reg = TCA9555_REG_INPUT1, in1 = 0;
    int i;

    (void)arg;
    for (;;) {
        TickType_t period = pdMS_TO_TICKS(BSP_BUTTON_POLL_PERIOD_MS);
        if (period < 1)
            period = 1;
        vTaskDelayUntil(&next, period);

        if (i2c_master_transmit_receive(s_dev, &reg, 1, &in1, 1,
                pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != ESP_OK)
            continue;

        for (i = 0; i < BSP_BUTTON_COUNT; i++) {
            int raw = ((in1 >> s_bits[i]) & 1) ? 0 : 1;
            if (raw == s_last_raw[i] && raw != s_last_stable[i]) {
                s_last_stable[i] = raw;
                s_button_dirty = 1;
            }
            s_last_raw[i] = raw;
        }
    }
}

void bsp_button_poll(void)
{
    int i;

    if (!s_button_dirty)
        return;

    s_button_dirty = 0;
    for (i = 0; i < BSP_BUTTON_COUNT; i++) {
        if (s_last_stable[i] != s_last_reported[i]) {
            s_last_reported[i] = s_last_stable[i];
            if (s_handler)
                s_handler(i, s_last_stable[i]);
        }
    }
}
