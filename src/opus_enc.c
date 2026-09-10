/*
 * Opus encoder wrapper: owns the encoder state and encode statistics.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <opus.h>

#include <zephyr/logging/log.h>

#include "audio_defs.h"
#include "opus_enc.h"

LOG_MODULE_REGISTER(opus_enc, LOG_LEVEL_INF);

static OpusEncoder *opus_enc;

static uint32_t opus_frame_count;
static uint64_t opus_bytes_total;
static uint32_t opus_encode_max_us;
static uint32_t opus_err_count;

int opus_enc_init(void)
{
	int err;

	opus_enc = opus_encoder_create(SAMPLE_RATE, CHANNEL_COUNT,
				       OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK || opus_enc == NULL) {
		LOG_ERR("opus_encoder_create failed: %d", err);
		return -EIO;
	}

	/* Default complexity (10) is far from real time on a 128 MHz M33;
	 * lower it for the demo. */
	opus_encoder_ctl(opus_enc,
			 OPUS_SET_COMPLEXITY(CONFIG_PDM_DEMO_OPUS_COMPLEXITY));

	LOG_INF("Opus encoder ready: state %d bytes, frame %u samples/ch (%u ms), "
		"complexity %d",
		opus_encoder_get_size(CHANNEL_COUNT), OPUS_FRAME_SAMPLES,
		CONFIG_PDM_DEMO_BLOCK_MS, CONFIG_PDM_DEMO_OPUS_COMPLEXITY);
	return 0;
}

int opus_enc_encode(const void *pcm, uint8_t *pkt, size_t cap)
{
#if defined(CONFIG_PDM_TEST_ONLY)
	ARG_UNUSED(pcm);
	ARG_UNUSED(pkt);
	ARG_UNUSED(cap);
	return 0;
#else
	uint32_t start_cycles = k_cycle_get_32();
	int nb_bytes = opus_encode(opus_enc, pcm, OPUS_FRAME_SAMPLES, pkt, cap);
	uint32_t encode_us =
		k_cyc_to_us_near32(k_cycle_get_32() - start_cycles);

	++opus_frame_count;

	if (nb_bytes < 0) {
		opus_err_count++;
		return nb_bytes;
	}

	opus_bytes_total += (uint32_t)nb_bytes;
	if (encode_us > opus_encode_max_us) {
		opus_encode_max_us = encode_us;
	}

	/* Per-frame timing for the first frames to gauge whether
	 * encoding keeps up with real time. */
	if (opus_frame_count <= 10U) {
		LOG_INF("Frame %u: %d bytes, enc %u us",
			opus_frame_count, nb_bytes, encode_us);
	}

	return nb_bytes;
#endif
}

void opus_enc_get_stats(uint64_t *bytes_total, uint32_t *max_us, uint32_t *err)
{
	*bytes_total = opus_bytes_total;
	*max_us = opus_encode_max_us;
	*err = opus_err_count;
}

int opus_enc_get_lookahead(void)
{
	int lookahead = 0;

#if !defined(CONFIG_PDM_TEST_ONLY)
	opus_encoder_ctl(opus_enc, OPUS_GET_LOOKAHEAD(&lookahead));
#endif
	return lookahead;
}

/* Self-init before main(); only needs the kernel heap. */
SYS_INIT(opus_enc_init, APPLICATION, 50);
