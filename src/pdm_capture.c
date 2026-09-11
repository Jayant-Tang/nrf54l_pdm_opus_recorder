/*
 * PDM capture module (Zephyr DMIC API on PDM20).
 *
 * PDM20 is connected to an external digital microphone:
 *   CLK: P1.12
 *   DIN: P1.11
 *
 * Data path: microphone -> PDM20 (DMA double-buffering into a memory
 * slab) -> pdm_capture_read() -> one AUDIO_BLOCK_SIZE PCM block per call.
 * Each block is exactly one Opus frame (see audio_defs.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>

#include "audio_defs.h"
#include "pdm_capture.h"

LOG_MODULE_REGISTER(pdm_capture, LOG_LEVEL_INF);

#define PDM_NODE	DT_NODELABEL(pdm20)
#define DMIC_NODE	DT_NODELABEL(pdm20_dmic)

#define READ_TIMEOUT_MS	1000

/* A sustained stall is this many consecutive read timeouts (~5 s). */
#define STALL_RESTART_THRESHOLD	5U

BUILD_ASSERT(DT_NODE_HAS_STATUS(PDM_NODE, okay), "PDM20 is not enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DMIC_NODE, okay), "PDM DMIC node is not enabled");
BUILD_ASSERT((AUDIO_BLOCK_SIZE % 4) == 0, "Audio block must be 4-byte aligned");

/* The sample rate is constrained to Opus-legal values by the Kconfig
 * choice, so no BUILD_ASSERT is needed here. */

/* DMA buffers: one slab block per driver queue entry. The devicetree
 * queue-size is the single source of truth: the driver caps its queue of
 * filled, unread blocks at queue-size, so a slab of the same size covers
 * the queue plus the DMA double-buffer while the consumer is stalled.
 * 16 x 20 ms = 320 ms of slack, sized to cover a LittleFS block erase
 * during recording (MX25R64 4 KB sector erase, max ~300 ms). */
#define AUDIO_BLOCK_COUNT	DT_PROP(PDM_NODE, queue_size)

K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_SIZE,
			 AUDIO_BLOCK_COUNT, 4);

static const struct device *const dmic_dev = DEVICE_DT_GET(PDM_NODE);

/* The DMIC driver has no PM support and applies the default pin state at
 * init, which connects the DIN input buffer: a powered microphone's
 * internal activity then leaks current into the SoC GPIO rail even while
 * PDM is off. Manage the pin states manually: sleep (input buffers
 * disconnected) whenever capture is stopped. The driver's own pinctrl
 * config is static to its compilation unit, so define a private copy
 * here (same DT node, a few bytes of duplicate ROM data). */
PINCTRL_DT_DEFINE(PDM_NODE);
static const struct pinctrl_dev_config *const pdm_pcfg =
	PINCTRL_DT_DEV_CONFIG_GET(PDM_NODE);

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

int pdm_capture_start(void)
{
	int ret;

	(void)pinctrl_apply_state(pdm_pcfg, PINCTRL_STATE_DEFAULT);

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

void pdm_capture_stop(void)
{
	int ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	if (ret < 0) {
		LOG_ERR("DMIC stop failed: %d", ret);
	}

	/* Drain queued blocks so the slab is whole for the next session. */
	void *buffer;
	size_t size;

	while (dmic_read(dmic_dev, 0, &buffer, &size, 0) == 0) {
		k_mem_slab_free(&audio_mem_slab, buffer);
	}

	(void)configure_dmic(false);
	(void)pinctrl_apply_state(pdm_pcfg, PINCTRL_STATE_SLEEP);
	LOG_INF("PDM capture stopped");
}

int pdm_capture_read(void **buf, size_t *size)
{
	static uint32_t stall_count;
	int ret;

	while (true) {
		ret = dmic_read(dmic_dev, 0, buf, size, READ_TIMEOUT_MS);
		if (ret == 0) {
			stall_count = 0;
			return 0;
		}

		if (ret != -EAGAIN) {
			LOG_ERR("DMIC read failed: %d", ret);
			return ret;
		}

		/* -EAGAIN: read timeout. The PDM driver stops permanently if
		 * the slab ever runs dry (e.g. a long flash erase stalls the
		 * consumer loop). Restart capture after a sustained stall
		 * instead of staying silent forever. */
		if (++stall_count >= STALL_RESTART_THRESHOLD) {
			LOG_WRN("PDM stalled, restarting capture");
			(void)configure_dmic(false);
			if (pdm_capture_start() == 0) {
				stall_count = 0;
			}
		}
	}
}

void pdm_capture_free(void *buf)
{
	k_mem_slab_free(&audio_mem_slab, buf);
}

int pdm_capture_init(void)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("PDM device is not ready");
		return -ENODEV;
	}

	/* DMIC init (POST_KERNEL) applied the default pin state; go to sleep
	 * state until the first recording starts. */
	(void)pinctrl_apply_state(pdm_pcfg, PINCTRL_STATE_SLEEP);
	return 0;
}

/* GPIO/pinctrl devices are ready from POST_KERNEL already. */
SYS_INIT(pdm_capture_init, APPLICATION, 80);
