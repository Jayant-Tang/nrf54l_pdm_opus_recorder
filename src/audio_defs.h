/*
 * Shared audio parameters for the PDM capture path (main.c) and the
 * Opus encoder (opus_enc.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_DEFS_H
#define AUDIO_DEFS_H

/* Shared by the PDM/DMIC path and the Opus encoder. */
#define SAMPLE_RATE        CONFIG_PDM_DEMO_SAMPLE_RATE
/* Both the DMIC driver and the opus_encode() s16 API are fixed at 16-bit. */
#define SAMPLE_BIT_WIDTH   16
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8)

#if CONFIG_PDM_DEMO_STEREO
#define CHANNEL_COUNT 2
#else
#define CHANNEL_COUNT 1
#endif

#define AUDIO_BLOCK_SIZE \
	((SAMPLE_RATE * BYTES_PER_SAMPLE * CHANNEL_COUNT * \
	  CONFIG_PDM_DEMO_BLOCK_MS) / 1000)

/* One DMA block is encoded as exactly one Opus frame. */
#define OPUS_FRAME_SAMPLES (AUDIO_BLOCK_SIZE / BYTES_PER_SAMPLE / CHANNEL_COUNT)
#define OPUS_MAX_PACKET    512

#endif /* AUDIO_DEFS_H */
