/*
 * Async I2C master — bus setup, worker task, completion delivery via poll().
 */

#include "espd_i2c.h"
#include "espd_config.h"
#include "espd_config_file.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <string.h>

#if CONFIG_ESPD_AUDIO_BACKEND_BSP_CODEC
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);
#endif

#ifdef ESPD_USE_I2C

static const char *TAG = "espd_i2c";

#ifndef ESPD_I2C_TASK_CORE
#define ESPD_I2C_TASK_CORE 0
#endif
#ifndef ESPD_I2C_TASK_PRIO
#define ESPD_I2C_TASK_PRIO 3
#endif
#ifndef ESPD_I2C_QUEUE_DEPTH
#define ESPD_I2C_QUEUE_DEPTH 8
#endif
#ifndef ESPD_I2C_XFER_TIMEOUT_MS
#define ESPD_I2C_XFER_TIMEOUT_MS 50
#endif
#ifndef ESPD_I2C_DEFAULT_FREQ_HZ
#define ESPD_I2C_DEFAULT_FREQ_HZ 100000
#endif

typedef struct {
    bool active;
    bool is_bsp;
    int sda;
    int scl;
    int freq_hz;
    i2c_master_bus_handle_t handle;
    SemaphoreHandle_t mutex;
} espd_i2c_bus_t;

typedef struct {
    void *owner;
    int bus;
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} espd_i2c_dev_slot_t;

#define ESPD_I2C_MAX_DEV_SLOTS 8

static espd_i2c_bus_t s_buses[ESPD_I2C_MAX_BUSES];
static espd_i2c_dev_slot_t s_devs[ESPD_I2C_MAX_DEV_SLOTS];
static QueueHandle_t s_job_q;
static QueueHandle_t s_done_q;
static TaskHandle_t s_task;
static espd_i2c_done_cb s_done_cb;
static int s_inited;

static espd_i2c_dev_slot_t *dev_slot_find_bus_addr(int bus, uint8_t addr)
{
    int i;
    for (i = 0; i < ESPD_I2C_MAX_DEV_SLOTS; i++) {
        if (s_devs[i].dev && s_devs[i].bus == bus && s_devs[i].addr == addr)
            return &s_devs[i];
    }
    return NULL;
}

void espd_i2c_set_done_handler(espd_i2c_done_cb cb)
{
    s_done_cb = cb;
}

void espd_i2c_poll(void)
{
    espd_i2c_job_t job;

    if (!s_done_q || !s_done_cb)
        return;
    while (xQueueReceive(s_done_q, &job, 0) == pdTRUE) {
        espd_i2c_dev_slot_t *slot = dev_slot_find_bus_addr(job.bus, job.addr);

        if (!slot || slot->owner != job.owner)
            continue;
        s_done_cb(&job);
    }
}

bool espd_i2c_bus_ready(int bus)
{
    if (bus < 0 || bus >= ESPD_I2C_MAX_BUSES)
        return false;
    return s_buses[bus].active;
}

int espd_i2c_bus_freq_hz(int bus)
{
    if (bus < 0 || bus >= ESPD_I2C_MAX_BUSES || !s_buses[bus].active)
        return 0;
    return s_buses[bus].freq_hz;
}

static espd_i2c_dev_slot_t *dev_slot_alloc(void *owner, int bus, uint8_t addr)
{
    espd_i2c_dev_slot_t *slot = dev_slot_find_bus_addr(bus, addr);
    int i;

    if (slot) {
        slot->owner = owner;
        return slot;
    }
    for (i = 0; i < ESPD_I2C_MAX_DEV_SLOTS; i++) {
        if (s_devs[i].dev == NULL && s_devs[i].owner == NULL) {
            s_devs[i].owner = owner;
            s_devs[i].bus = bus;
            s_devs[i].addr = addr;
            return &s_devs[i];
        }
    }
    return NULL;
}

static esp_err_t dev_ensure(espd_i2c_dev_slot_t *slot, espd_i2c_bus_t *bus)
{
    i2c_device_config_t cfg;

    if (slot->dev)
        return ESP_OK;
    if (!bus->handle)
        return ESP_ERR_INVALID_STATE;

    cfg = (i2c_device_config_t){
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = slot->addr,
        .scl_speed_hz = (uint32_t)bus->freq_hz,
    };
    return i2c_master_bus_add_device(bus->handle, &cfg, &slot->dev);
}

void espd_i2c_release_device(void *owner)
{
    int i;

    for (i = 0; i < ESPD_I2C_MAX_DEV_SLOTS; i++) {
        espd_i2c_bus_t *bus;

        if (s_devs[i].owner != owner)
            continue;
        bus = (s_devs[i].bus >= 0 && s_devs[i].bus < ESPD_I2C_MAX_BUSES &&
               s_buses[s_devs[i].bus].active)
            ? &s_buses[s_devs[i].bus] : NULL;
        if (bus && bus->mutex)
            xSemaphoreTake(bus->mutex, portMAX_DELAY);
        s_devs[i].owner = NULL;
        if (bus && bus->mutex)
            xSemaphoreGive(bus->mutex);
    }
}

static esp_err_t run_job(espd_i2c_job_t *job)
{
    espd_i2c_bus_t *bus;
    espd_i2c_dev_slot_t *slot;
    esp_err_t err;

    if (job->bus < 0 || job->bus >= ESPD_I2C_MAX_BUSES ||
        !s_buses[job->bus].active)
        return ESP_ERR_INVALID_ARG;

    bus = &s_buses[job->bus];
    slot = dev_slot_alloc(job->owner, job->bus, job->addr);
    if (!slot)
        return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(bus->mutex, pdMS_TO_TICKS(ESPD_I2C_XFER_TIMEOUT_MS)) !=
        pdTRUE)
        return ESP_ERR_TIMEOUT;

    err = dev_ensure(slot, bus);
    if (err != ESP_OK) {
        xSemaphoreGive(bus->mutex);
        return err;
    }

    switch (job->op) {
    case ESPD_I2C_OP_READ:
        if (job->read_len == 0 || job->read_len > ESPD_I2C_MAX_READ) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = i2c_master_transmit_receive(
            slot->dev, &job->reg, 1, job->read_buf, job->read_len,
            ESPD_I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK)
            job->read_len_actual = job->read_len;
        break;

    case ESPD_I2C_OP_WRITE:
        if (job->write_len == 0 ||
            job->write_len + 1 > ESPD_I2C_MAX_WRITE) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        {
            uint8_t buf[ESPD_I2C_MAX_WRITE];
            buf[0] = job->reg;
            memcpy(buf + 1, job->write_buf, job->write_len);
            err = i2c_master_transmit(slot->dev, buf, job->write_len + 1,
                ESPD_I2C_XFER_TIMEOUT_MS);
        }
        break;

    case ESPD_I2C_OP_WRITE_RAW:
        if (job->write_len == 0 || job->write_len > ESPD_I2C_MAX_WRITE) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = i2c_master_transmit(slot->dev, job->write_buf, job->write_len,
            ESPD_I2C_XFER_TIMEOUT_MS);
        break;

    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    xSemaphoreGive(bus->mutex);
    return err;
}

static void espd_i2c_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        espd_i2c_job_t job;
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE)
            continue;
        job.result = run_job(&job);
        if (xQueueSend(s_done_q, &job, 0) != pdTRUE)
            ESP_LOGW(TAG, "done queue full — dropping I2C result");
    }
}

bool espd_i2c_submit(espd_i2c_job_t *job)
{
    if (!s_inited || !job || !s_job_q)
        return false;
    if (xQueueSend(s_job_q, job, 0) != pdTRUE)
        return false;
    return true;
}

static esp_err_t bus_create_user(int idx, int sda, int scl, int freq_hz)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = (idx == 0) ? I2C_NUM_0 : I2C_NUM_1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    espd_i2c_bus_t *bus = &s_buses[idx];

    if (i2c_new_master_bus(&cfg, &bus->handle) != ESP_OK)
        return ESP_FAIL;
    bus->active = true;
    bus->is_bsp = false;
    bus->sda = sda;
    bus->scl = scl;
    bus->freq_hz = freq_hz;
    bus->mutex = xSemaphoreCreateMutex();
    if (!bus->mutex) {
        i2c_del_master_bus(bus->handle);
        memset(bus, 0, sizeof(*bus));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "bus%d: user SDA=%d SCL=%d %d Hz", idx, sda, scl, freq_hz);
    return ESP_OK;
}

static esp_err_t bus_attach_bsp(int idx, int freq_hz)
{
#if CONFIG_ESPD_AUDIO_BACKEND_BSP_CODEC
    i2c_master_bus_handle_t h = bsp_i2c_get_handle();
#else
    i2c_master_bus_handle_t h = NULL;
#endif
    espd_i2c_bus_t *bus = &s_buses[idx];

    if (!h)
        return ESP_ERR_INVALID_STATE;
    bus->handle = h;
    bus->active = true;
    bus->is_bsp = true;
    bus->sda = -1;
    bus->scl = -1;
    bus->freq_hz = freq_hz;
    bus->mutex = xSemaphoreCreateMutex();
    if (!bus->mutex) {
        memset(bus, 0, sizeof(*bus));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "bus%d: BSP shared bus %d Hz", idx, freq_hz);
    return ESP_OK;
}

void espd_i2c_init(void)
{
    int have = 0;

    if (s_inited)
        return;

    if (g_espd_cfg.i2c_bsp) {
        int freq = (g_espd_cfg.i2c_bsp_freq_hz > 0)
            ? g_espd_cfg.i2c_bsp_freq_hz : 400000;
        if (bus_attach_bsp(0, freq) == ESP_OK)
            have++;
        else
            ESP_LOGW(TAG, "i2c_bsp=1 but BSP bus unavailable");
    }

    if (g_espd_cfg.i2c_have[0]) {
        if (!s_buses[0].active) {
            if (bus_create_user(0, g_espd_cfg.i2c_sda[0],
                    g_espd_cfg.i2c_scl[0], g_espd_cfg.i2c_freq_hz[0]) == ESP_OK)
                have++;
        } else {
            ESP_LOGW(TAG, "i2c0= ignored (bus 0 already active via i2c_bsp)");
        }
    }

    if (g_espd_cfg.i2c_have[1]) {
        if (bus_create_user(1, g_espd_cfg.i2c_sda[1], g_espd_cfg.i2c_scl[1],
                g_espd_cfg.i2c_freq_hz[1]) == ESP_OK)
            have++;
    }

    if (!have) {
        ESP_LOGI(TAG, "no I2C buses configured");
        return;
    }

    s_job_q = xQueueCreate(ESPD_I2C_QUEUE_DEPTH, sizeof(espd_i2c_job_t));
    s_done_q = xQueueCreate(ESPD_I2C_QUEUE_DEPTH, sizeof(espd_i2c_job_t));
    if (!s_job_q || !s_done_q) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }

    if (xTaskCreatePinnedToCore(espd_i2c_task_fn, "espd_i2c", 3072, NULL,
            ESPD_I2C_TASK_PRIO, &s_task, ESPD_I2C_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to create worker task");
        return;
    }

    s_inited = 1;
    ESP_LOGI(TAG, "worker on core %d (%d bus(es))", (int)ESPD_I2C_TASK_CORE,
        have);
}

#else /* !ESPD_USE_I2C */

void espd_i2c_init(void) {}
void espd_i2c_set_done_handler(espd_i2c_done_cb cb)
{
    (void)cb;
}
void espd_i2c_poll(void) {}
bool espd_i2c_bus_ready(int bus)
{
    (void)bus;
    return false;
}
int espd_i2c_bus_freq_hz(int bus)
{
    (void)bus;
    return 0;
}
bool espd_i2c_submit(espd_i2c_job_t *job)
{
    (void)job;
    return false;
}
void espd_i2c_release_device(void *owner)
{
    (void)owner;
}

#endif /* ESPD_USE_I2C */
