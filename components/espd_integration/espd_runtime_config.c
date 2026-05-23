#include "espd_runtime_config.h"

static int s_dma_desc_num = ESPD_AUDIO_DMA_DESC_NUM_DEFAULT;
static int s_dma_frame_num = ESPD_AUDIO_DMA_FRAME_NUM_DEFAULT;

int espd_audio_dma_desc_num(void)
{
    return s_dma_desc_num;
}

int espd_audio_dma_frame_num(void)
{
    return s_dma_frame_num;
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
