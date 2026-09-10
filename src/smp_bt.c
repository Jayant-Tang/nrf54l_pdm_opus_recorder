/*
 * BLE SMP advertising for the MCUMgr file management demo.
 *
 * Modelled on zephyr/samples/subsys/mgmt/mcumgr/smp_svr/src/bluetooth.c:
 * advertise the SMP service UUID, restart advertising once the previous
 * connection object is recycled. Unauthenticated (no pairing).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/init.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>

#include <zephyr/logging/log.h>

#include "smp_bt.h"

LOG_MODULE_REGISTER(smp_bt, LOG_LEVEL_INF);

static struct k_work advertise_work;

/* 200 ms interval (in 0.625 ms units): much lower idle current than
 * BT_LE_ADV_CONN_FAST_1 (~50 ms), still connects quickly enough. */
#define ADV_INTERVAL_200MS	0x140

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise(struct k_work *work)
{
	int rc = bt_le_adv_start(
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADV_INTERVAL_200MS,
				ADV_INTERVAL_200MS, NULL),
		ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (rc) {
		LOG_ERR("Advertising failed to start (rc %d)", rc);
		return;
	}

	LOG_INF("SMP advertising started (name \"%s\")", CONFIG_BT_DEVICE_NAME);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err,
			bt_hci_err_to_str(err));
		k_work_submit(&advertise_work);
	} else {
		LOG_INF("BLE connected");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("BLE disconnected, reason 0x%02x %s", reason,
		bt_hci_err_to_str(reason));
}

static void on_conn_recycled(void)
{
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(smp_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = on_conn_recycled,
};

static void bt_ready(int err)
{
	if (err != 0) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}

	k_work_submit(&advertise_work);
}

int smp_bt_start(void)
{
	k_work_init(&advertise_work, advertise);
	return bt_enable(bt_ready);
}

/* Self-init; independent of the recorder/encoder init order. */
SYS_INIT(smp_bt_start, APPLICATION, 70);
