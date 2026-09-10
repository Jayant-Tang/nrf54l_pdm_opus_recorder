/*
 * Opus recorder on LittleFS (external flash, auto-mounted at /lfs1).
 *
 * Encoded frames are encapsulated with libogg into standard Ogg Opus
 * (.opus) files, directly playable by VLC / browsers / phones after
 * download (e.g. via mcumgr).
 *
 * One button press records CONFIG_PDM_DEMO_REC_SECONDS into one file;
 * file names increment (rec_0000.opus, rec_0001.opus, ...).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <zephyr/fs/fs.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <ogg/ogg.h>

#include "audio_defs.h"
#include "opus_enc.h"
#include "recorder.h"

LOG_MODULE_REGISTER(recorder, LOG_LEVEL_INF);

#define REC_MOUNT_POINT "/lfs1"
#define REC_FRAMES \
	((uint32_t)CONFIG_PDM_DEMO_REC_SECONDS * 1000 / CONFIG_PDM_DEMO_BLOCK_MS)

/* External flash and its SPI bus. The spi_nor driver puts both back to
 * sleep after EVERY operation (unconditional pm_device_runtime_put), so
 * without a reference held here each fs op during recording would pay a
 * deep-power-down exit and starve the real-time capture loop. */
#define REC_FLASH_NODE DT_NODELABEL(mx25r64)
static const struct device *const rec_flash = DEVICE_DT_GET(REC_FLASH_NODE);
static const struct device *const rec_spi =
	DEVICE_DT_GET(DT_BUS(REC_FLASH_NODE));

/* Ogg granule position runs on a 48 kHz clock regardless of the input rate. */
#define GP_PER_FRAME ((int64_t)OPUS_FRAME_SAMPLES * 48000 / SAMPLE_RATE)

static struct fs_file_t rec_file;
static ogg_stream_state ogg_stream;
static bool recording;
static uint32_t frames_left;
static uint32_t file_index;
static uint32_t cur_file_size;
static int64_t granulepos;
static int64_t packetno;
static char cur_path[32];

/* Pick up after the highest existing rec_XXXX.opus so a reboot does not
 * overwrite older clips. */
static void scan_file_index(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, REC_MOUNT_POINT) < 0) {
		return;
	}

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type == FS_DIR_ENTRY_FILE &&
		    strncmp(ent.name, "rec_", 4) == 0) {
			long idx = strtol(ent.name + 4, NULL, 10);

			if (idx >= 0 && (uint32_t)idx >= file_index) {
				file_index = (uint32_t)idx + 1;
			}
		}
	}
	(void)fs_closedir(&dir);
}

/* Rewrite /lfs1/index.txt with the current recording list. The SMP fs
 * group has no "list directory" command and the iOS Device Manager app
 * has no shell, so clients discover recordings by downloading this
 * fixed-path file first. */
static void update_index(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	struct fs_file_t idx;
	char line[48];

	/* No truncate flag in Zephyr fs; unlink first so a shorter new
	 * index cannot leave a stale tail. */
	(void)fs_unlink(REC_MOUNT_POINT "/index.txt");

	fs_file_t_init(&idx);
	if (fs_open(&idx, REC_MOUNT_POINT "/index.txt",
		    FS_O_CREATE | FS_O_WRITE) < 0) {
		LOG_WRN("index.txt create failed");
		return;
	}

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, REC_MOUNT_POINT) == 0) {
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
			if (ent.type == FS_DIR_ENTRY_FILE &&
			    strncmp(ent.name, "rec_", 4) == 0) {
				int len = snprintk(line, sizeof(line),
						   "%9u  %s\n",
						   (unsigned int)ent.size,
						   ent.name);

				if (len > 0) {
					(void)fs_write(&idx, line, len);
				}
			}
		}
		(void)fs_closedir(&dir);
	}
	(void)fs_close(&idx);
}

int recorder_init(void)
{
	struct fs_statvfs stat;

	int ret = fs_statvfs(REC_MOUNT_POINT, &stat);

	if (ret < 0) {
		LOG_ERR("%s not mounted: %d", REC_MOUNT_POINT, ret);
		return ret;
	}

	LOG_INF("%s ready: %lu KB total, %lu KB free", REC_MOUNT_POINT,
		(unsigned long)(stat.f_blocks * stat.f_bsize / 1024),
		(unsigned long)(stat.f_bfree * stat.f_bsize / 1024));

	scan_file_index();
	update_index();
	return 0;
}

/* Drain completed Ogg pages to the file. */
static int write_pages(bool flush)
{
	ogg_page og;

	while (flush ? ogg_stream_flush(&ogg_stream, &og) :
		       ogg_stream_pageout(&ogg_stream, &og)) {
		uint32_t c0 = k_cycle_get_32();
		int rc = fs_write(&rec_file, og.header, og.header_len);
		int rc2 = (rc < 0) ? rc : fs_write(&rec_file, og.body,
						   og.body_len);
		uint32_t us = (uint32_t)((uint64_t)(k_cycle_get_32() - c0) *
					 1000000U / sys_clock_hw_cycles_per_sec());

		if (us > 20000U) {
			LOG_WRN("slow fs_write: %u us (hdr %u body %u)", us,
				og.header_len, og.body_len);
		}
		if (rc < 0 || rc2 < 0) {
			return -EIO;
		}
		cur_file_size += og.header_len + og.body_len;
	}
	return 0;
}

/* OpusHead + OpusTags, each flushed onto its own page as required. */
static int write_opus_headers(void)
{
	static const char vendor[] = "nrf54l_pdm_opus_recorder";
	uint8_t head[19];
	uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
	ogg_packet op;

	memcpy(head, "OpusHead", 8);
	head[8] = 1;              /* version */
	head[9] = CHANNEL_COUNT;
	sys_put_le16((uint16_t)opus_enc_get_lookahead(), &head[10]);
	sys_put_le32(SAMPLE_RATE, &head[12]);
	sys_put_le16(0, &head[16]); /* output gain */
	head[18] = 0;             /* channel mapping family 0 */

	memset(&op, 0, sizeof(op));
	op.packet = head;
	op.bytes = sizeof(head);
	op.b_o_s = 1;
	ogg_stream_packetin(&ogg_stream, &op);

	memcpy(tags, "OpusTags", 8);
	sys_put_le32(sizeof(vendor) - 1, &tags[8]);
	memcpy(&tags[12], vendor, sizeof(vendor) - 1);
	sys_put_le32(0, &tags[12 + sizeof(vendor) - 1]); /* no comments */

	memset(&op, 0, sizeof(op));
	op.packet = tags;
	op.bytes = sizeof(tags);
	ogg_stream_packetin(&ogg_stream, &op);

	packetno = 2;
	return write_pages(true);
}

int recorder_start(void)
{
	char path[32];
	int ret;

	if (recording) {
		return -EBUSY;
	}

	/* Pin flash + SPI bus active for the whole clip. */
	(void)pm_device_runtime_get(rec_flash);
	(void)pm_device_runtime_get(rec_spi);

	snprintk(path, sizeof(path), "%s/rec_%04u.opus", REC_MOUNT_POINT,
		 file_index);
	fs_file_t_init(&rec_file);
	ret = fs_open(&rec_file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("open %s failed: %d", path, ret);
		goto out_pm_put;
	}

	ret = ogg_stream_init(&ogg_stream,
			      (int)(file_index ^ k_uptime_get_32()));
	if (ret != 0) {
		LOG_ERR("ogg_stream_init failed: %d", ret);
		(void)fs_close(&rec_file);
		ret = -ENOMEM;
		goto out_pm_put;
	}

	strncpy(cur_path, path, sizeof(cur_path));
	cur_path[sizeof(cur_path) - 1] = '\0';
	cur_file_size = 0;
	granulepos = 0;
	frames_left = REC_FRAMES;
	recording = true;

	ret = write_opus_headers();
	if (ret < 0) {
		LOG_ERR("write headers failed");
		recording = false;
		ogg_stream_clear(&ogg_stream);
		(void)fs_close(&rec_file);
		goto out_pm_put;
	}

	LOG_INF("Recording %s (%u s, %u frames)", cur_path,
		CONFIG_PDM_DEMO_REC_SECONDS, REC_FRAMES);
	return 0;

out_pm_put:
	(void)pm_device_runtime_put(rec_spi);
	(void)pm_device_runtime_put(rec_flash);
	return ret;
}

static void recorder_stop(void)
{
	(void)write_pages(true); /* flush the partial last page */
	ogg_stream_clear(&ogg_stream);
	(void)fs_sync(&rec_file);
	(void)fs_close(&rec_file);
	recording = false;
	file_index++;
	LOG_INF("Saved %s, %u bytes", cur_path, cur_file_size);
	update_index();
	(void)pm_device_runtime_put(rec_spi);
	(void)pm_device_runtime_put(rec_flash);
}

void recorder_feed_frame(const uint8_t *data, uint16_t len)
{
	ogg_packet op;

	if (!recording) {
		return;
	}

	granulepos += GP_PER_FRAME;

	memset(&op, 0, sizeof(op));
	op.packet = (unsigned char *)data;
	op.bytes = len;
	op.granulepos = granulepos;
	op.packetno = packetno++;
	op.e_o_s = (frames_left == 1) ? 1 : 0;

	ogg_stream_packetin(&ogg_stream, &op);

	if (write_pages(false) < 0) {
		LOG_ERR("write failed, aborting recording");
		recorder_stop();
		return;
	}

	if (--frames_left == 0) {
		recorder_stop();
	}
}

bool recorder_is_recording(void)
{
	return recording;
}

/* Self-init at APPLICATION level: LittleFS automount already ran at
 * POST_KERNEL (CONFIG_FILE_SYSTEM_INIT_PRIORITY). */
SYS_INIT(recorder_init, APPLICATION, 60);
