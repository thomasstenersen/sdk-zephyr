/* main.c - Application main entry point */

/*
 * Copyright (c) 2015 Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Intel Corporation nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>
#include <misc/printk.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <misc/byteorder.h>

#define SLEEPTIME  5000
#define SLEEPTICKS (SLEEPTIME * sys_clock_ticks_per_sec / 1000)

static struct bt_conn *default_conn;

static struct bt_uuid hrs = {
		.type = BT_UUID_16,
		.u16 = BT_UUID_HRS,
};

static struct bt_uuid hrm = {
		.type = BT_UUID_16,
		.u16 = BT_UUID_HRS_MEASUREMENT,
};

static struct bt_uuid ccc = {
		.type = BT_UUID_16,
		.u16 = BT_UUID_GATT_CCC,
};

static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;

static void subscribe_func(struct bt_conn *conn, int err,
			   const void *data, uint16_t length)
{
	if (length) {
		printk("[NOTIFICATION] data %p length %u\n", data, length);
	}
}

static uint8_t discover_func(const struct bt_gatt_attr *attr, void *user_data)
{
	int err;

	printk("[ATTRIBUTE] handle %u\n", attr->handle);

	if (discover_params.uuid == &hrs) {
		discover_params.uuid = &hrm;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}
	} else if (discover_params.uuid == &hrm) {
		discover_params.uuid = &ccc;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
		subscribe_params.value_handle = attr->handle + 1;

		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}

	} else {
		subscribe_params.func = subscribe_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;

		err = bt_gatt_subscribe(default_conn, attr->handle,
					&subscribe_params);
		if (err && err != -EALREADY) {
			printk("Subscribe failed (err %d)\n", err);
		}
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}

static void connected(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Connected: %s\n", addr);

	if (conn == default_conn) {
		discover_params.uuid = &hrs;
		discover_params.func = discover_func;
		discover_params.start_handle = 0x0001;
		discover_params.end_handle = 0xffff;
		discover_params.type = BT_GATT_DISCOVER_PRIMARY;

		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			printk("Discover failed(err %d)\n", err);
			return;
		}
	}
}

static bool eir_found(const struct bt_eir *eir, void *user_data)
{
	bt_addr_le_t *addr = user_data;
	uint16_t u16;
	int i;

	printk("[AD]: %u len %u\n", eir->type, eir->len);

	switch (eir->type) {
	case BT_EIR_UUID16_SOME:
	case BT_EIR_UUID16_ALL:
		if ((eir->len - sizeof(eir->type)) % sizeof(u16) != 0) {
			printk("AD malformed\n");
			return true;
		}

		for (i = 0; i < eir->len; i += sizeof(u16)) {
			memcpy(&u16, &eir->data[i], sizeof(u16));
			if (sys_le16_to_cpu(u16) == BT_UUID_HRS) {
				int err = bt_stop_scanning();

				if (err) {
					printk("Stopping scanning failed"
						" (err %d)\n", err);
				}

				default_conn = bt_conn_create_le(addr);
				return false;
			}
		}
	}

	return true;
}

static void ad_parse(const uint8_t *data, uint8_t len,
		     bool (*func)(const struct bt_eir *eir, void *user_data),
		     void *user_data)
{
	const void *p;

	for (p = data; len > 0;) {
		const struct bt_eir *eir = p;

		/* Check for early termination */
		if (eir->len == 0) {
			return;
		}

		if ((eir->len + sizeof(eir->len) > len) ||
		    (len < sizeof(eir->len) + sizeof(eir->type))) {
			printk("AD malformed\n");
			return;
		}

		if (!func(eir, user_data)) {
			return;
		}

		p += sizeof(eir->len) + eir->len;
		len -= sizeof(eir->len) + eir->len;
	}
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 const uint8_t *ad, uint8_t len)
{
	char dev[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, dev, sizeof(dev));
	printk("[DEVICE]: %s, AD evt type %u, AD data len %u, RSSI %i\n",
	       dev, type, len, rssi);

	/* TODO: Move this to a place it can be shared */
	ad_parse(ad, len, eir_found, (void *)addr);
}

static void disconnected(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s\n", addr);

	if (default_conn != conn) {
		return;
	}

	bt_conn_put(default_conn);
	default_conn = NULL;

	err = bt_start_scanning(BT_SCAN_FILTER_DUP_DISABLE, device_found);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
	}
}

static struct bt_conn_cb conn_callbacks = {
		.connected = connected,
		.disconnected = disconnected,
};

#ifdef CONFIG_MICROKERNEL
void mainloop(void)
#else
void main(void)
#endif
{
	int err;
	err = bt_enable(NULL);

	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	bt_conn_cb_register(&conn_callbacks);

	err = bt_start_scanning(BT_SCAN_FILTER_DUP_ENABLE, device_found);

	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

	printk("Scanning successfully started\n");

	while (1) {
		task_sleep(SLEEPTICKS);
	}
}

