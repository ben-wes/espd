/*
 * Run-time tuning loaded from config.txt before audio init (see main/espd.c).
 */
#pragma once

#ifndef ESPD_AUDIO_DMA_DESC_NUM_DEFAULT
#define ESPD_AUDIO_DMA_DESC_NUM_DEFAULT 3
#endif
#ifndef ESPD_AUDIO_DMA_FRAME_NUM_DEFAULT
#define ESPD_AUDIO_DMA_FRAME_NUM_DEFAULT 64
#endif
/* Must match Pd DEFDACBLKSIZE / espd.c BLKSIZE (one I2S write per block). */
#ifndef ESPD_AUDIO_DMA_FRAME_NUM_PD_BLOCK
#define ESPD_AUDIO_DMA_FRAME_NUM_PD_BLOCK 64
#endif

/** I2S DMA descriptor count (IDF minimum 2). */
int espd_audio_dma_desc_num(void);

/** I2S frames per DMA buffer; latency ≈ desc_num * frame_num / sample_rate. */
int espd_audio_dma_frame_num(void);

void espd_audio_set_dma_desc_num(int desc_num);
void espd_audio_set_dma_frame_num(int frame_num);
