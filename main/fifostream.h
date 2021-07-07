// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
// All rights reserved.

#ifndef _FIFOSTREAM_H_
#define _FIFOSTREAM_H_

#include "esp_err.h"
#include "audio_element.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief      Configuration
 */
typedef struct fifostream_cfg {
    int samplerate;     /*!< Audio sample rate (in Hz)*/
    int channel;        /*!< Number of audio channels (Mono=1, Dual=2) */
    int *set_gain;      /*!< Equalizer gain */
    int out_rb_size;    /*!< Size of output ring buffer */
    int task_stack;     /*!< Task stack size */
    int task_core;      /*!< Task running in core...*/
    int task_prio;      /*!< Task priority*/
    bool stack_in_ext;  /*!< Try to allocate stack in external memory */
} fifostream_cfg_t;

#define FIFOSTREAM_TASK_STACK       (20000)
#define FIFOSTREAM_TASK_CORE        (0)
#define FIFOSTREAM_TASK_PRIO        (5)
#define FIFOSTREAM_RINGBUFFER_SIZE  (8 * 1024)

/**
* @note  `set_value_gain` is defined in c file.
*         values is {-13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13};
*/
extern int set_value_gain[];

#define DEFAULT_FIFOSTREAM_CONFIG() {                    \
        .samplerate     = 48000,                        \
        .channel        = 1,                            \
        .set_gain       = set_value_gain,               \
        .out_rb_size    = FIFOSTREAM_RINGBUFFER_SIZE,    \
        .task_stack     = FIFOSTREAM_TASK_STACK,         \
        .task_core      = FIFOSTREAM_TASK_CORE,          \
        .task_prio      = FIFOSTREAM_TASK_PRIO,          \
        .stack_in_ext   = true,                         \
    }
    
audio_element_handle_t fifostream_init(fifostream_cfg_t *config);

#ifdef __cplusplus
}
#endif

#endif
