/*
 * nrf54l_pdm_opus_recorder — nRF54L15/LM20 DK PDM Opus recorder
 *
 * This file is only the demo state machine; each building block lives in
 * its own module so they can be read independently:
 *
 *   pdm_capture.c  PDM20/DMIC capture (PCM blocks)
 *   opus_enc.c     Opus encoder wrapper
 *   recorder.c     Ogg Opus files on LittleFS (external flash)
 *   smp_bt.c       BLE SMP (MCUMgr) file transfer
 *
 * PDM stays off at idle. Pressing Button 0 starts capture and records
 * Opus audio to /lfs1/rec_XXXX.opus; pressing it again (or reaching
 * CONFIG_PDM_DEMO_REC_SECONDS) stops and saves. LED0 is on while
 * recording. Long-pressing Button 1 deletes all recordings.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_PDM_DEMO_GAIN_DB > 0
#include <math.h>
#endif

#include <dk_buttons_and_leds.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>

#include "audio_defs.h"
#include "opus_enc.h"
#include "pdm_capture.h"
#include "recorder.h"

LOG_MODULE_REGISTER(pdm_opus_recorder, LOG_LEVEL_INF);

/* prj_pdm_only.conf is a pure PDM + Opus power baseline: no storage,
 * no LED, no BLE. Everything file/LED related is compiled out there. */
#define RECORDER_ENABLED	(!IS_ENABLED(CONFIG_PDM_DEMO_PDM_ONLY))

#if !RECORDER_ENABLED
BUILD_ASSERT(!IS_ENABLED(CONFIG_FILE_SYSTEM) && !IS_ENABLED(CONFIG_BT),
	     "pdm_only must not enable storage or BLE");
#endif

/* --- Button 0 / LED 0 (NCS DK library, debounce built in) --- */

#define REC_BTN_EVT	BIT(0)
#define DEL_BTN_EVT	BIT(1)

/* The DK library numbers buttons/LEDs from 1 (an nRF52 DK legacy),
 * while the nRF54L DK silkscreen and devicetree number them from 0:
 * DK_BTN1 / DK_LED1 are the on-board Button 0 / LED 0. */
#define REC_BTN_MSK	DK_BTN1_MSK
#define REC_LED		DK_LED1

/* Long-press Button 1 (DK library DK_BTN2) to delete all recordings. */
#define DEL_BTN_MSK		DK_BTN2_MSK
#define DEL_LED			DK_LED2
#define DEL_LONG_PRESS_MS	3000

static K_EVENT_DEFINE(rec_events);

/* Desired recording state, toggled by the debounced button callback
 * (system workqueue context); the main loop turns it into actual
 * capture start/stop. */
static volatile bool rec_active;

/* Long-press detection: the debounced DK handler only reports edges,
 * so a press arms a delayed work and a release cancels it; if it
 * expires, the button has been held for DEL_LONG_PRESS_MS. */
static void del_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(del_work, del_work_handler);

static void del_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_event_post(&rec_events, DEL_BTN_EVT);
}

static void button_handler(uint32_t state, uint32_t changed)
{
	if ((changed & REC_BTN_MSK) && (state & REC_BTN_MSK)) {
		rec_active = !rec_active;
#if RECORDER_ENABLED
		/* LED0 follows the recording state. */
		(void)dk_set_led(REC_LED, rec_active);
#endif
		k_event_post(&rec_events, REC_BTN_EVT);
	}

	if (changed & DEL_BTN_MSK) {
		if (state & DEL_BTN_MSK) {
			k_work_reschedule(&del_work,
					  K_MSEC(DEL_LONG_PRESS_MS));
		} else {
			(void)k_work_cancel_delayable(&del_work);
		}
	}
}

/* LED is part of the storage demo only; the pdm_only configuration is a
 * power baseline and keeps it off. */
static void rec_led_off(void)
{
#if RECORDER_ENABLED
	(void)dk_set_led_off(REC_LED);
#endif
}

#if RECORDER_ENABLED
/* Delete all recordings (long-press path). LED1 is on while erasing. */
static void delete_all_recordings(void)
{
	int ret;

	(void)dk_set_led_on(DEL_LED);
	ret = recorder_delete_all();
	(void)dk_set_led_off(DEL_LED);

	if (ret >= 0) {
		LOG_INF("Deleted all recordings (%d file(s))", ret);
	} else {
		LOG_WRN("Delete all failed: %d", ret);
	}
}
#endif

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

/* Fixed-point Q16 multiply with int16 saturation. Gain must live before
 * compression: Opus packets cannot be scaled linearly.
 * The product is computed in int64: with int32 the multiply can overflow
 * (signed overflow is UB), which lets the compiler legally optimize the
 * CLAMP away entirely - loud signals then wrap around instead of
 * saturating. */
static void apply_gain(int16_t *samples, size_t size)
{
	for (size_t i = 0; i < size / sizeof(*samples); i++) {
		int32_t s = (int32_t)(((int64_t)samples[i] * gain_q16 +
				       0x8000) >> 16);

		samples[i] = (int16_t)CLAMP(s, INT16_MIN, INT16_MAX);
	}
}
#else
static void gain_init(void) { }
#endif

/* --- Recording chain helpers --- */

/* Start the whole chain: PDM capture + file recording. */
static int recording_start(void)
{
	if (pdm_capture_start() < 0) {
		return -EIO;
	}
#if RECORDER_ENABLED
	if (recorder_start() < 0) {
		pdm_capture_stop();
		return -EIO;
	}
#endif
	return 0;
}

/* Stop PDM first (no new frames), then flush the file. */
static void recording_stop(void)
{
	pdm_capture_stop();
#if RECORDER_ENABLED
	/* No-op when the CONFIG_PDM_DEMO_REC_SECONDS cap already closed
	 * the file. */
	recorder_stop();
#endif
}

static void log_stats(uint32_t block_count)
{
	uint64_t bytes_total;
	uint32_t max_us, err;

	opus_enc_get_stats(&bytes_total, &max_us, &err);
	LOG_INF("Blocks %u, opus avg %llu B/frame, max enc %u us, err %u",
		block_count, bytes_total / block_count, max_us, err);
}

static void capture_loop(void)
{
	static uint8_t opus_packet[OPUS_MAX_PACKET];

	while (true) {
		/* Idle: PDM off, sleep until the button toggles rec_active
		 * or a long press requests a delete-all. A delete request
		 * posted during recording is handled once the clip ends. */
		uint32_t evt = k_event_wait(&rec_events,
					    REC_BTN_EVT | DEL_BTN_EVT,
					    true, K_FOREVER);

#if RECORDER_ENABLED
		if (evt & DEL_BTN_EVT) {
			delete_all_recordings();
		}
#else
		ARG_UNUSED(evt);
#endif
		if (!rec_active) {
			continue; /* stop press while idle: ignore */
		}

		if (recording_start() < 0) {
			rec_active = false;
			rec_led_off();
			continue;
		}

		uint32_t block_count = 0;

		while (rec_active) {
			void *buffer;
			size_t size;

			if (pdm_capture_read(&buffer, &size) < 0) {
				break;
			}

#if CONFIG_PDM_DEMO_GAIN_DB > 0
			apply_gain(buffer, size);
#endif
			/* One PCM block is exactly one Opus frame. */
			int nb = opus_enc_encode(buffer, opus_packet,
						 sizeof(opus_packet));

#if RECORDER_ENABLED
			if (nb > 0) {
				recorder_feed_frame(opus_packet, (uint16_t)nb);
			}
#else
			ARG_UNUSED(nb);
#endif
			pdm_capture_free(buffer);

			if ((++block_count % 100U) == 0U) {
				log_stats(block_count);
			}

#if RECORDER_ENABLED
			/* The recorder closes the file by itself at the
			 * CONFIG_PDM_DEMO_REC_SECONDS cap. */
			if (!recorder_is_recording()) {
				break;
			}
#endif
		}

		recording_stop();
		rec_active = false;
		rec_led_off();
	}
}

int main(void)
{
	gain_init();

	/* Buttons and LEDs come from the NCS DK library (debounce built
	 * in; the handler runs in the system workqueue). */
#if RECORDER_ENABLED
	if (dk_leds_init() != 0) {
		LOG_WRN("LED init failed, no recording indicator");
	}
#endif
	if (dk_buttons_init(button_handler) != 0) {
		LOG_ERR("Button init failed");
		return 0;
	}

	/* opus_enc / recorder / smp_bt / pdm_capture all self-init via
	 * SYS_INIT (APPLICATION level, priorities 50/60/70/80). */
	LOG_INF("%s PDM Opus recorder ready", CONFIG_BOARD);
	LOG_INF("PDM20 CLK=P1.12 DIN=P1.11, PCM=%u Hz/%u-bit",
		CONFIG_PDM_DEMO_SAMPLE_RATE, SAMPLE_BIT_WIDTH);
	LOG_INF("Press Button 0 to start/stop recording");
#if RECORDER_ENABLED
	LOG_INF("Long-press Button 1 (%u ms) to delete all recordings",
		DEL_LONG_PRESS_MS);
#endif

	capture_loop();

	return 0;
}
