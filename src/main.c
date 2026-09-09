/*
 * learning_zephyr_pdm — nRF54L15DK PDM power demo
 *
 * PDM20 is connected to an external digital microphone:
 *   CLK: P1.12
 *   DIN: P1.11
 *
 * Capture starts automatically and runs until reset.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <opus.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pdm_power_demo, LOG_LEVEL_INF);

#define PDM_NODE       DT_NODELABEL(pdm20)
#define DMIC_NODE      DT_NODELABEL(pdm20_dmic)

/* Shared by the PDM/DMIC path and the Opus encoder. */
#define SAMPLE_RATE        CONFIG_PDM_DEMO_SAMPLE_RATE
/* Both the DMIC driver and the opus_encode() s16 API are fixed at 16-bit. */
#define SAMPLE_BIT_WIDTH   16
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8)
#define READ_TIMEOUT_MS    1000

#if CONFIG_PDM_DEMO_STEREO
#define CHANNEL_COUNT 2
#else
#define CHANNEL_COUNT 1
#endif

#define AUDIO_BLOCK_SIZE \
	((SAMPLE_RATE * BYTES_PER_SAMPLE * CHANNEL_COUNT * \
	  CONFIG_PDM_DEMO_BLOCK_MS) / 1000)

BUILD_ASSERT(DT_NODE_HAS_STATUS(PDM_NODE, okay), "PDM20 is not enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DMIC_NODE, okay), "PDM DMIC node is not enabled");
BUILD_ASSERT((AUDIO_BLOCK_SIZE % 4) == 0, "Audio block must be 4-byte aligned");

/* Opus only accepts 8/12/16/24/48 kHz. */
BUILD_ASSERT(SAMPLE_RATE == 8000 || SAMPLE_RATE == 12000 ||
	     SAMPLE_RATE == 16000 || SAMPLE_RATE == 24000 ||
	     SAMPLE_RATE == 48000,
	     "Opus sample rate must be 8000, 12000, 16000, 24000 or 48000 Hz");

K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_SIZE,
			 CONFIG_PDM_DEMO_BLOCK_COUNT, 4);

static const struct device *const dmic_dev = DEVICE_DT_GET(PDM_NODE);

static void build_dmic_config(struct dmic_cfg *cfg,
			      struct pcm_stream_cfg *stream,
			      bool enabled)
{
	*stream = (struct pcm_stream_cfg) {
		.pcm_rate = enabled ? SAMPLE_RATE : 0,
		.pcm_width = enabled ? SAMPLE_BIT_WIDTH : 0,
		.block_size = AUDIO_BLOCK_SIZE,
		.mem_slab = &audio_mem_slab,
	};

	*cfg = (struct dmic_cfg) {
		.io = PDM_DT_IO_CFG_GET(DMIC_NODE),
		.streams = stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = CHANNEL_COUNT,
		},
	};

#if CONFIG_PDM_DEMO_STEREO
	cfg->channel.req_chan_map_lo =
		dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
		dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
#elif CONFIG_PDM_DEMO_MONO_RIGHT
	cfg->channel.req_chan_map_lo =
		dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT);
#else
	cfg->channel.req_chan_map_lo =
		dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
#endif
}

static int configure_dmic(bool enabled)
{
	struct dmic_cfg cfg;
	struct pcm_stream_cfg stream;

	build_dmic_config(&cfg, &stream, enabled);
	return dmic_configure(dmic_dev, &cfg);
}

static int start_capture(void)
{
	int ret;

	ret = configure_dmic(true);
	if (ret < 0) {
		LOG_ERR("DMIC configure failed: %d", ret);
		return ret;
	}

	ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("DMIC start failed: %d", ret);
		(void)configure_dmic(false);
		return ret;
	}

	LOG_INF("PDM capture started (%s)", IS_ENABLED(CONFIG_PDM_DEMO_STEREO) ?
		"stereo" : (IS_ENABLED(CONFIG_PDM_DEMO_MONO_RIGHT) ?
		"mono-right" : "mono-left"));

	return 0;
}


/* One DMA block is encoded as exactly one Opus frame. */
#define OPUS_FRAME_SAMPLES (AUDIO_BLOCK_SIZE / BYTES_PER_SAMPLE / CHANNEL_COUNT)
#define OPUS_MAX_PACKET    512

/* The block duration is constrained to legal Opus frame sizes by the
 * Kconfig choice, so no BUILD_ASSERT is needed here. */

static OpusEncoder *opus_enc;
static uint8_t opus_packet[OPUS_MAX_PACKET];

static uint32_t opus_frame_count;
static uint64_t opus_bytes_total;
static uint32_t opus_encode_max_us;
static uint32_t opus_err_count;

static int opus_setup(void)
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

/* No-op under CONFIG_PDM_TEST_ONLY: the block is just returned. */
static void opus_encode_block(const void *pcm)
{
#if !defined(CONFIG_PDM_TEST_ONLY)
	uint32_t start_cycles = k_cycle_get_32();
	int nb_bytes = opus_encode(opus_enc, pcm, OPUS_FRAME_SAMPLES,
				   opus_packet, sizeof(opus_packet));
	uint32_t encode_us =
		k_cyc_to_us_near32(k_cycle_get_32() - start_cycles);

	++opus_frame_count;

	if (nb_bytes < 0) {
		opus_err_count++;
		return;
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
#endif
	ARG_UNUSED(pcm);
}

static void log_stats(uint32_t block_count, size_t last_size)
{
#if defined(CONFIG_PDM_TEST_ONLY)
	LOG_INF("Received %u PCM blocks, last size %u bytes",
		block_count, last_size);
#else
	ARG_UNUSED(last_size);

	LOG_INF("Blocks %u, opus avg %llu B/frame, max enc %u us, err %u",
		block_count, opus_bytes_total / block_count,
		opus_encode_max_us, opus_err_count);
#endif
}

static void capture_forever(void)
{
	uint32_t block_count = 0;

	while (true) {
		void *buffer;
		size_t size;
		int ret;

		ret = dmic_read(dmic_dev, 0, &buffer, &size, READ_TIMEOUT_MS);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret != 0) {
			LOG_ERR("DMIC read failed: %d", ret);
			return;
		}

		opus_encode_block(buffer);
		k_mem_slab_free(&audio_mem_slab, buffer);
		++block_count;

		if ((block_count % 100U) == 0U) {
			log_stats(block_count, size);
		}
	}
}

int main(void)
{
	int ret;

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Required device is not ready");
		return 0;
	}

	LOG_INF("%s PDM power demo ready", CONFIG_BOARD);
	LOG_INF("PDM20 CLK=P1.12 DIN=P1.11, PCM=%u Hz/%u-bit",
		CONFIG_PDM_DEMO_SAMPLE_RATE, SAMPLE_BIT_WIDTH);
	LOG_INF("PDM capture starts automatically");

	ret = opus_setup();
	if (ret < 0) {
		return 0;
	}

	ret = start_capture();
	if (ret < 0) {
		return 0;
	}

	capture_forever();

	return 0;
}
