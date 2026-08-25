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
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pdm_power_demo, LOG_LEVEL_INF);

#if CONFIG_PDM_DEMO_ENABLE

#define PDM_NODE       DT_NODELABEL(pdm20)
#define DMIC_NODE      DT_NODELABEL(pdm20_dmic)

#define SAMPLE_RATE        16000
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

static void capture_forever(void)
{
	uint32_t block_count = 0;

	while (true) {
		void *buffer;
		size_t size;
		int ret;

		ret = dmic_read(dmic_dev, 0, &buffer, &size, READ_TIMEOUT_MS);

        
		if (ret == 0) {
			k_mem_slab_free(&audio_mem_slab, buffer);
			++block_count;

			if ((block_count % 100U) == 0U) {
				LOG_INF("Received %u PCM blocks, last size %u bytes",
					block_count, size);
			}
		} else if (ret != -EAGAIN) {
			LOG_ERR("DMIC read failed: %d", ret);
			return;
		}
	}
}

#endif /* CONFIG_PDM_DEMO_ENABLE */

int main(void)
{
#if CONFIG_PDM_DEMO_ENABLE
	int ret;

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Required device is not ready");
		return 0;
	}

	LOG_INF("nRF54L15DK PDM power demo ready");
	LOG_INF("PDM20 CLK=P1.12 DIN=P1.11, PCM=16 kHz/16-bit");
	LOG_INF("PDM capture starts automatically");

	ret = start_capture();
	if (ret < 0) {
		return 0;
	}

	capture_forever();
#else
	/* Idle image: do not initialize or start any application peripheral. */
#endif

	return 0;
}
