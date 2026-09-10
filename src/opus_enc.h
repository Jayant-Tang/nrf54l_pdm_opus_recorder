/*
 * Opus encoder wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OPUS_ENC_H
#define OPUS_ENC_H

#include <stddef.h>
#include <stdint.h>

/* Create the encoder and apply CONFIG_PDM_DEMO_OPUS_COMPLEXITY. */
int opus_enc_init(void);

/* Encode one PCM block (AUDIO_BLOCK_SIZE bytes) into pkt.
 * Returns the encoded byte count or a negative Opus error code. */
int opus_enc_encode(const void *pcm, uint8_t *pkt, size_t cap);

void opus_enc_get_stats(uint64_t *bytes_total, uint32_t *max_us, uint32_t *err);

/* Encoder lookahead in samples, for the OpusHead pre-skip field. */
int opus_enc_get_lookahead(void);

#endif /* OPUS_ENC_H */
