/*
 * Opus frame recorder on LittleFS (external flash).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RECORDER_H
#define RECORDER_H

#include <stdbool.h>
#include <stdint.h>

/* Check the auto-mounted /lfs1 volume and find the next free file index. */
int recorder_init(void);

/* Start recording into /lfs1/rec_XXXX.opus (Ogg Opus, directly playable).
 * Returns -EBUSY while a previous clip is still being recorded. */
int recorder_start(void);

/* Flush and close the current clip (no-op when not recording). Called
 * automatically at the CONFIG_PDM_DEMO_REC_SECONDS cap; call it to stop
 * early (button). */
void recorder_stop(void);

/* Feed one encoded Opus frame; written only while recording. The file is
 * synced and closed automatically after CONFIG_PDM_DEMO_REC_SECONDS. */
void recorder_feed_frame(const uint8_t *data, uint16_t len);

bool recorder_is_recording(void);

/* Delete every rec_*.opus on /lfs1 and reset the file index to 0.
 * Returns the number of deleted files, or -EBUSY while recording. */
int recorder_delete_all(void);

#endif /* RECORDER_H */
