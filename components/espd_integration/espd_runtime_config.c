#include <espd_runtime_config.h>
#include <sdkconfig.h>

#ifndef CONFIG_ESPD_AUDIO_SAMPLE_RATE
#define CONFIG_ESPD_AUDIO_SAMPLE_RATE 48000
#endif

static int s_dma_desc_num = ESPD_AUDIO_DMA_DESC_NUM_DEFAULT;
static int s_dma_frame_num = ESPD_AUDIO_DMA_FRAME_NUM_DEFAULT;
static int s_sample_rate_hz = CONFIG_ESPD_AUDIO_SAMPLE_RATE;

int espd_audio_dma_desc_num(void)
{
    return s_dma_desc_num;
}

int espd_audio_dma_frame_num(void)
{
    return s_dma_frame_num;
}

int espd_audio_sample_rate_hz(void)
{
    return s_sample_rate_hz;
}

void espd_audio_set_dma_desc_num(int desc_num)
{
    if (desc_num >= 2 && desc_num <= 16)
        s_dma_desc_num = desc_num;
}

void espd_audio_set_dma_frame_num(int frame_num)
{
    if (frame_num >= 8 && frame_num <= 1024)
        s_dma_frame_num = frame_num;
}

void espd_audio_set_sample_rate(int hz)
{
    if (hz >= ESPD_AUDIO_SAMPLE_RATE_MIN && hz <= ESPD_AUDIO_SAMPLE_RATE_MAX)
        s_sample_rate_hz = hz;
}
