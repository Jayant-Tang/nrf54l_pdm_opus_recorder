/*
 * PDM capture module (Zephyr DMIC API on PDM20).
 *
 * This file is the "PDM" chapter of the demo: it owns everything between
 * the digital microphone and a plain PCM buffer - the DMIC driver
 * configuration, the DMA memory slab, the pin power states and the
 * stall recovery - so that main.c only sees start/read/stop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PDM_CAPTURE_H
#define PDM_CAPTURE_H

#include <stddef.h>

/* Apply the sleep pin state (input buffers disconnected) until the first
 * capture starts. Runs automatically via SYS_INIT. */
int pdm_capture_init(void);

/* Power up the pins, configure the DMIC and start DMA capture. */
int pdm_capture_start(void);

/* Stop DMA, drain queued blocks back to the slab and put the pins to
 * sleep. Safe to call once per pdm_capture_start(). */
void pdm_capture_stop(void);

/* Fetch the next PCM block (AUDIO_BLOCK_SIZE bytes, blocking).
 * If the driver stalls because the slab ran dry (e.g. a long flash erase
 * starved the loop), capture is restarted automatically; the caller only
 * sees a later block. Returns 0 on success, a negative error code when
 * the driver reported a fatal error. */
int pdm_capture_read(void **buf, size_t *size);

/* Return a block obtained from pdm_capture_read() to the slab. */
void pdm_capture_free(void *buf);

#endif /* PDM_CAPTURE_H */
