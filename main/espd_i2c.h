/*
 * Async I2C master — Core 0 worker, completions on the Pd/audio thread.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define ESPD_I2C_MAX_BUSES 2
#define ESPD_I2C_MAX_READ  32
#define ESPD_I2C_MAX_WRITE 32

typedef enum {
    ESPD_I2C_OP_READ = 0,
    ESPD_I2C_OP_WRITE,
    ESPD_I2C_OP_WRITE_RAW,
} espd_i2c_op_t;

typedef struct espd_i2c_job espd_i2c_job_t;

struct espd_i2c_job {
    void *owner;
    int bus;
    uint8_t addr;
    espd_i2c_op_t op;
    uint8_t reg;
    uint8_t write_len;
    uint8_t read_len;
    uint8_t write_buf[ESPD_I2C_MAX_WRITE];
    esp_err_t result;
    uint8_t read_buf[ESPD_I2C_MAX_READ];
    uint8_t read_len_actual;
};

#include <driver/i2c_master.h>

typedef void (*espd_i2c_done_cb)(espd_i2c_job_t *job);

void espd_i2c_init(void);
void espd_i2c_set_done_handler(espd_i2c_done_cb cb);
void espd_i2c_poll(void);
bool espd_i2c_bus_ready(int bus);

/* Enqueue a job; returns false if queue full (caller should report busy). */
bool espd_i2c_submit(espd_i2c_job_t *job);

void espd_i2c_release_device(void *owner);
