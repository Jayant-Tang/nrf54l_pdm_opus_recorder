/*
 * BLE SMP advertising for the MCUMgr file management demo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SMP_BT_H
#define SMP_BT_H

/* Enable Bluetooth and start advertising the SMP service. Returns 0 if
 * advertising was (asynchronously) scheduled, a negative error code if
 * Bluetooth failed to initialise. File management stays available over
 * UART even when this fails. */
int smp_bt_start(void);

#endif /* SMP_BT_H */
