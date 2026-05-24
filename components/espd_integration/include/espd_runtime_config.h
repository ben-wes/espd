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

/* Hard limits for runtime sample rate. Defaults come from Kconfig
 * (CONFIG_ESPD_AUDIO_SAMPLE_RATE); config.txt may override at boot. */
#ifndef ESPD_AUDIO_SAMPLE_RATE_MIN
#define ESPD_AUDIO_SAMPLE_RATE_MIN 8000
#endif
#ifndef ESPD_AUDIO_SAMPLE_RATE_MAX
#define ESPD_AUDIO_SAMPLE_RATE_MAX 192000
#endif

/** I2S DMA descriptor count (IDF minimum 2). */
int espd_audio_dma_desc_num(void);

/** I2S frames per DMA buffer; latency ≈ desc_num * frame_num / sample_rate. */
int espd_audio_dma_frame_num(void);

/** Single source of truth for sample rate (Hz). Same value flows into the
 *  audio backend (I2S / codec) and into Pd via sys_getsr(). */
int espd_audio_sample_rate_hz(void);

void espd_audio_set_dma_desc_num(int desc_num);
void espd_audio_set_dma_frame_num(int frame_num);

/** Override the sample rate before audio init (config.txt / app boot). */
void espd_audio_set_sample_rate(int hz);
