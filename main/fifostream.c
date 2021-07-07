// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
// All rights reserved.

#include <string.h>
#include "esp_log.h"
#include "audio_error.h"
#include "audio_mem.h"
#include "audio_element.h"
#include "fifostream.h"
#include "audio_type_def.h"
static const char *TAG = "fifostream";

#define CHANS 2
#define INCHANS CHANS
#define OUTCHANS CHANS
#define BLKSIZE 64
#define BLK_BYTES (BLKSIZE*CHANS*sizeof(short))

extern void pdmain_tick( void);

typedef struct fifostream {
    int  samplerate;
    int  channel;
    short *s_buf;
    int  byte_num;
    int  at_eof;
} fifostream_t;

static esp_err_t fifostream_destroy(audio_element_handle_t self)
{
    fifostream_t *fifostream = (fifostream_t *)audio_element_getdata(self);
    audio_free(fifostream);
    return ESP_OK;
}

static esp_err_t fifostream_open(audio_element_handle_t self)
{
    ESP_LOGD(TAG, "fifostream_open");
    fifostream_t *fifostream = (fifostream_t *)audio_element_getdata(self);
    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    if (info.sample_rates
        && info.channels) {
        fifostream->samplerate = info.sample_rates;
        fifostream->channel = info.channels;
    }
    fifostream->at_eof = 0;
    fifostream->s_buf = (short *)calloc(sizeof(short), BLK_BYTES/sizeof(short));
    if (fifostream->s_buf == NULL) {
        ESP_LOGE(TAG, "calloc buffer failed. (line %d)", __LINE__);
        return ESP_ERR_NO_MEM;
    }
    memset(fifostream->s_buf, 0, BLK_BYTES);

    return ESP_OK;
}

static esp_err_t fifostream_close(audio_element_handle_t self)
{
    ESP_LOGD(TAG, "fifostream_close");
    fifostream_t *fifostream = (fifostream_t *)audio_element_getdata(self);
    if (fifostream->s_buf == NULL) {
        audio_free(fifostream->s_buf);
        fifostream->s_buf = NULL;
    }
    return ESP_OK;
}

float soundin[OUTCHANS * BLKSIZE], soundout[OUTCHANS * BLKSIZE];

#if 0   /* better one that doesn't work for some reason */
static int fifostream_process(audio_element_handle_t self, char *in_buffer,
    int in_len)
{
    static int cumsamps = 0;
    fifostream_t *fifostream = (fifostream_t *)audio_element_getdata(self);
    int ret = 0;
    int r_size = (fifostream->at_eof ? 0 :
        audio_element_input(self, (char *)fifostream->s_buf, BLK_BYTES));
    if (r_size >= BLK_BYTES)
    {
        int i, nsamps = BLKSIZE;
        fifostream->byte_num += BLK_BYTES;
        
        for (i = 0; i < BLKSIZE; i+= 2)
        {
            soundin[i] = fifostream->s_buf[i]/32768;;
            soundin[BLKSIZE+i] = fifostream->s_buf[i+1]/32768;
        }
        pdmain_tick();
        for (i = 0; i < BLKSIZE; i+= 2)
        {
            int ch1 = 32767*soundout[i], ch2 = 32767*soundout[BLKSIZE+i];
            if (ch1 > 32767)
                ch1 = 32767;
            else if (ch1 < -32768)
                ch1 = -32768;
            if (ch2 > 32767)
                ch2 = 32767;
            else if (ch2 < -32768)
                ch2 = -32768;
            fifostream->s_buf[i] = (short)ch1;
            fifostream->s_buf[i+1] = (short)ch2;
        }
        ret = audio_element_output(self, (char *)fifostream->s_buf, BLK_BYTES);
    }
    else
    {
        fifostream->at_eof = 1;
        ret = 0;
    }
    return ret;
}
#endif

#if 1
static int fifostream_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    static int cumsamps = 0;
    fifostream_t *fifostream = (fifostream_t *)audio_element_getdata(self);
    int ret = 0;
    int r_size = 0;
    if (fifostream->at_eof == 0) {
        r_size = audio_element_input(self, (char *)fifostream->s_buf, BLK_BYTES);
    }
    if (r_size > 0) {
        if (r_size != BLK_BYTES) {
            fifostream->at_eof = 1;
        }
        fifostream->byte_num += r_size;
        {
            int i, nsamps = r_size/(2*sizeof(short));
            static int phase;
            if (nsamps * 2 * sizeof(short) != r_size)
                ESP_LOGE("WOMBAT", "odd sample frame size %d", r_size);
            for (i = 0; i < nsamps; i++)
            {
                int ch1 = 32767*soundout[i], ch2 = 32767*soundout[BLKSIZE+i];
                if (ch1 > 32767)
                    ch1 = 32767;
                else if (ch1 < -32768)
                    ch1 = -32768;
                if (ch2 > 32767)
                    ch2 = 32767;
                else if (ch2 < -32768)
                ch2 = -32768;
                soundin[phase] = fifostream->s_buf[2*i]/32768;
                fifostream->s_buf[2*i] = ch1;
                soundin[BLKSIZE+phase] = fifostream->s_buf[2*i+1]/32768;
                fifostream->s_buf[2*i+1] = ch2;
                phase++;
                if (phase == BLKSIZE)
                {
                    pdmain_tick();
                    phase = 0;
                }
            }
        }
        ret = audio_element_output(self, (char *)fifostream->s_buf, BLK_BYTES);
    } else {
        ret = r_size;
    }
    return ret;
}
#endif

audio_element_handle_t fifostream_init(fifostream_cfg_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "config is NULL. (line %d)", __LINE__);
        return NULL;
    }
    fifostream_t *fifostream = audio_calloc(1, sizeof(fifostream_t));
    AUDIO_MEM_CHECK(TAG, fifostream, return NULL);
    if (fifostream == NULL) {
        ESP_LOGE(TAG, "audio_calloc failed for fifostream. (line %d)", __LINE__);
        return NULL;
    }
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.destroy = fifostream_destroy;
    cfg.process = fifostream_process;
    cfg.open = fifostream_open;
    cfg.close = fifostream_close;
    cfg.buffer_len = 0;
    cfg.tag = "fifostream";
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.stack_in_ext = config->stack_in_ext;
    audio_element_handle_t el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, {audio_free(fifostream); return NULL;});
    fifostream->samplerate = config->samplerate;
    fifostream->channel = config->channel;
    audio_element_setdata(el, fifostream);
    audio_element_info_t info = {0};
    audio_element_setinfo(el, &info);
    ESP_LOGD(TAG, "fifostream_init");
    return el;
}
