/*
 * nrf54l_pdm_opus_recorder — nRF54L15/LM20 DK PDM Opus recorder
 *
 * PDM20 is connected to an external digital microphone:
 *   CLK: P1.12
 *   DIN: P1.11
 *
 * PDM stays off at idle. Pressing Button 0 starts capture and records
 * Opus audio to a file on the external flash (LittleFS, /lfs1);
 * pressing it again (or reaching CONFIG_PDM_DEMO_REC_SECONDS) stops and
 * saves. LED0 is on while recording.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_PDM_DEMO_GAIN_DB > 0
#include <math.h>
#endif

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>

#include "audio_defs.h"
#include "opus_enc.h"
#include "recorder.h"

LOG_MODULE_REGISTER(pdm_opus_recorder, LOG_LEVEL_INF);

#define PDM_NODE       DT_NODELABEL(pdm20)
#define DMIC_NODE      DT_NODELABEL(pdm20_dmic)

#define READ_TIMEOUT_MS    1000

BUILD_ASSERT(DT_NODE_HAS_STATUS(PDM_NODE, okay), "PDM20 is not enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DMIC_NODE, okay), "PDM DMIC node is not enabled");
BUILD_ASSERT((AUDIO_BLOCK_SIZE % 4) == 0, "Audio block must be 4-byte aligned");

/* The sample rate is constrained to Opus-legal values by the Kconfig
 * choice, so no BUILD_ASSERT is needed here. */

K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_SIZE,
			 CONFIG_PDM_DEMO_BLOCK_COUNT, 4);

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

static int start_capture(void)
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

static void stop_capture(void)
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

/* --- Button 0: trigger a recording (port event via sense-edge-mask) --- */

#define REC_BTN_EVT	BIT(0)
/* Ignore edges for this long after a trigger (contact bounce). */
#define BTN_DEBOUNCE_MS	200

static K_EVENT_DEFINE(rec_events);

static const struct gpio_dt_spec rec_button =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback rec_button_cb;
static int64_t last_trigger_ms;

static void button_isr(const struct device *dev, struct gpio_callback *cb,
		       uint32_t pins)
{
	k_event_post(&rec_events, REC_BTN_EVT);
}

static int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&rec_button)) {
		LOG_ERR("Button 0 device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&rec_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Button 0 configure failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&rec_button,
					      GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Button 0 interrupt configure failed: %d", ret);
		return ret;
	}

	gpio_init_callback(&rec_button_cb, button_isr, BIT(rec_button.pin));
	gpio_add_callback_dt(&rec_button, &rec_button_cb);
	return 0;
}

/* --- Recording status LED (led0): on while a clip is being written --- */

static const struct gpio_dt_spec rec_led =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
#if defined(CONFIG_FILE_SYSTEM)
static bool rec_led_on;
#endif

static int led_init(void)
{
	if (!gpio_is_ready_dt(&rec_led)) {
		LOG_WRN("LED0 not available, no recording indicator");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&rec_led, GPIO_OUTPUT_INACTIVE);

	if (ret < 0) {
		LOG_ERR("LED0 configure failed: %d", ret);
	}
	return ret;
}

/* Cheap enough to call every loop iteration; only touches the GPIO on
 * state changes. */
static void update_rec_led(void)
{
#if defined(CONFIG_FILE_SYSTEM)
	bool recording = recorder_is_recording();

	if (recording != rec_led_on) {
		(void)gpio_pin_set_dt(&rec_led, recording);
		rec_led_on = recording;
	}
#endif
}

/* --- PCM gain (applied before Opus encoding) --- */

#if CONFIG_PDM_DEMO_GAIN_DB > 0
static int32_t gain_q16 = 65536; /* Q16, 1.0x */

static void gain_init(void)
{
	gain_q16 = (int32_t)(pow(10.0, CONFIG_PDM_DEMO_GAIN_DB / 20.0) *
			     65536.0);
	LOG_INF("PCM gain %d dB (x%u.%02u)", CONFIG_PDM_DEMO_GAIN_DB,
		(unsigned)(gain_q16 >> 16),
		(unsigned)((gain_q16 & 0xFFFF) * 100 / 65536));
}

static void apply_gain(int16_t *samples, size_t size)
{
	for (size_t i = 0; i < size / sizeof(*samples); i++) {
		int32_t s = ((int32_t)samples[i] * gain_q16 + 0x8000) >> 16;

		samples[i] = (int16_t)CLAMP(s, INT16_MIN, INT16_MAX);
	}
}
#else
static void gain_init(void) { }
#endif

static int board_io_init(void)
{
	gain_init();
	(void)button_init();
	(void)led_init();
	/* DMIC init (POST_KERNEL) applied the default pin state; go to sleep
	 * state until the first recording starts. */
	(void)pinctrl_apply_state(pdm_pcfg, PINCTRL_STATE_SLEEP);
	return 0;
}

/* Consume a pending button event with debounce; true on a valid press. */
static bool button_pressed(void)
{
	if (k_event_test(&rec_events, REC_BTN_EVT) == 0) {
		return false;
	}
	k_event_clear(&rec_events, REC_BTN_EVT);

	int64_t now = k_uptime_get();

	if (now - last_trigger_ms < BTN_DEBOUNCE_MS) {
		return false;
	}
	last_trigger_ms = now;
	return true;
}

static void log_stats(uint32_t block_count, size_t last_size)
{
	uint64_t bytes_total;
	uint32_t max_us, err;

	ARG_UNUSED(last_size);

	opus_enc_get_stats(&bytes_total, &max_us, &err);
	LOG_INF("Blocks %u, opus avg %llu B/frame, max enc %u us, err %u",
		block_count, bytes_total / block_count, max_us, err);
}

/* Phase timing for stall diagnosis (cycles converted to us). */
static uint32_t max_feed_us, max_misc_us;

static inline uint32_t cycles_to_us(uint32_t cycles)
{
	return (uint32_t)((uint64_t)cycles * 1000000U /
			  sys_clock_hw_cycles_per_sec());
}

static void capture_loop(void)
{
	static uint8_t opus_packet[OPUS_MAX_PACKET];
	uint32_t block_count;
	uint32_t stall_count;

	while (true) {
		/* Idle: PDM off, sleep until a debounced button press. */
		while (!button_pressed()) {
			k_event_wait(&rec_events, REC_BTN_EVT, false,
				     K_FOREVER);
		}

		if (start_capture() < 0) {
			continue;
		}
#if defined(CONFIG_FILE_SYSTEM)
		if (recorder_start() < 0) {
			stop_capture();
			continue;
		}
#endif
		update_rec_led();
		block_count = 0;
		stall_count = 0;

		while (true) {
			void *buffer;
			size_t size;
			int ret;

			ret = dmic_read(dmic_dev, 0, &buffer, &size,
					READ_TIMEOUT_MS);
			if (ret == -EAGAIN) {
				/* The PDM driver stops permanently if the
				 * slab ever runs dry (e.g. a long flash
				 * erase stalls the loop). Restart capture
				 * after a sustained stall instead of
				 * staying silent forever. */
				if (++stall_count >= 5U) {
					LOG_WRN("PDM stalled, restarting "
						"capture (max feed %u us, "
						"max misc %u us)",
						max_feed_us, max_misc_us);
					max_feed_us = 0;
					max_misc_us = 0;
					(void)configure_dmic(false);
					if (start_capture() == 0) {
						stall_count = 0;
					}
				}
			} else if (ret != 0) {
				LOG_ERR("DMIC read failed: %d", ret);
				break;
			} else {
				stall_count = 0;

#if CONFIG_PDM_DEMO_GAIN_DB > 0
				apply_gain(buffer, size);
#endif

				int nb = opus_enc_encode(buffer, opus_packet,
							 sizeof(opus_packet));

				uint32_t c0 = k_cycle_get_32();

#if defined(CONFIG_FILE_SYSTEM)
				if (nb > 0) {
					recorder_feed_frame(opus_packet,
							    (uint16_t)nb);
				}
#else
				ARG_UNUSED(nb);
#endif

				uint32_t c1 = k_cycle_get_32();

				k_mem_slab_free(&audio_mem_slab, buffer);
				++block_count;

				update_rec_led();

				uint32_t c2 = k_cycle_get_32();
				uint32_t feed_us = cycles_to_us(c1 - c0);
				uint32_t misc_us = cycles_to_us(c2 - c1);

				if (feed_us > max_feed_us) {
					max_feed_us = feed_us;
				}
				if (misc_us > max_misc_us) {
					max_misc_us = misc_us;
				}

				if ((block_count % 100U) == 0U) {
					log_stats(block_count, size);
				}
			}

			/* Stop on a second press, or when the recorder hit
			 * the CONFIG_PDM_DEMO_REC_SECONDS cap and closed the
			 * file by itself. */
			if (button_pressed()) {
				break;
			}
#if defined(CONFIG_FILE_SYSTEM)
			if (!recorder_is_recording()) {
				break;
			}
#endif
		}

		/* Stop PDM first (no new frames), then flush the file. */
		stop_capture();
#if defined(CONFIG_FILE_SYSTEM)
		if (recorder_is_recording()) {
			recorder_stop();
		}
#endif
		update_rec_led();
	}
}

int main(void)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Required device is not ready");
		return 0;
	}

	/* opus_enc / recorder / smp_bt / button+led all self-init via
	 * SYS_INIT (APPLICATION level, priorities 50/60/70/80). */
	LOG_INF("%s PDM Opus recorder ready", CONFIG_BOARD);
	LOG_INF("PDM20 CLK=P1.12 DIN=P1.11, PCM=%u Hz/%u-bit",
		CONFIG_PDM_DEMO_SAMPLE_RATE, SAMPLE_BIT_WIDTH);
	LOG_INF("Press Button 0 to start/stop recording");

	capture_loop();

	return 0;
}

/* After recorder (60) and smp_bt (70); GPIO devices are ready from
 * POST_KERNEL already. */
SYS_INIT(board_io_init, APPLICATION, 80);
