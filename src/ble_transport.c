#include "ble_transport.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/settings/settings.h>

#include <bluetooth/services/nus.h>

#define FIXED_PASSKEY 444999u

static struct bt_conn *current_conn;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void advertising_start(void)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                              sd, ARRAY_SIZE(sd));
    if (err == 0) {
        printk("BLE advertising (re)started\n");
    } else if (err != -EALREADY) {
        printk("bt_le_adv_start failed: %d\n", err);
    }
}

void ble_transport_ensure_advertising(void)
{
    if (current_conn == NULL) {
        advertising_start();
    }
}

/* Run advertising restart from the system workqueue rather than directly
 * from disconnected(). When called inline from the BT RX thread before the
 * host has finished tearing down the just-closed connection, bt_le_adv_start
 * returns -ENOMEM. Submitting to a worker decouples timing so the start
 * succeeds on the first try.
 */
static void adv_restart_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (current_conn == NULL) {
        advertising_start();
    }
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        printk("BLE connect failed (%s): 0x%02x\n", addr, err);
        return;
    }

    current_conn = bt_conn_ref(conn);
    printk("BLE connected: %s\n", addr);

    /* Require encrypted + authenticated (passkey/MITM) link before doing
     * anything useful. Pair-less centrals will fail and disconnect.
     */
    int sec = bt_conn_set_security(conn, BT_SECURITY_L3);
    if (sec) {
        printk("bt_conn_set_security failed: %d\n", sec);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE disconnected: %s (reason 0x%02x)\n", addr, reason);

    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    k_work_submit(&adv_restart_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        printk("BLE security failed (%s): level %u, err 0x%02x\n",
               addr, level, err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    printk("BLE security raised (%s): level %u\n", addr, level);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static uint32_t auth_app_passkey(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE passkey for %s: %06u\n", addr, FIXED_PASSKEY);
    return FIXED_PASSKEY;
}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE pairing cancelled: %s\n", addr);
}

static const struct bt_conn_auth_cb auth_cb = {
    .app_passkey = auth_app_passkey,
    .cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE pairing complete: %s (bonded=%d)\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE pairing failed: %s (reason 0x%02x)\n", addr, reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

static void nus_received(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    ARG_UNUSED(conn);
    printk("NUS RX (%u):", len);
    for (uint16_t i = 0; i < len; i++) {
        printk(" %02X", data[i]);
    }
    printk("\n");
}

static struct bt_nus_cb nus_callbacks = {
    .received = nus_received,
};

/* ---- Log backend: mirrors log/printk traffic over NUS notifications. ---- */

#define NUS_LOG_BUF_SIZE 128
static uint8_t nus_log_buf[NUS_LOG_BUF_SIZE];

static int nus_log_out(uint8_t *data, size_t length, void *ctx)
{
    ARG_UNUSED(ctx);
    if (current_conn) {
        /* Drop the chunk on failure (e.g. notifications not yet enabled or
         * BT TX queue full); the USB CDC backend still has the full record.
         */
        (void)bt_nus_send(current_conn, data, length);
    }
    return length;
}

LOG_OUTPUT_DEFINE(nus_log_output, nus_log_out, nus_log_buf, sizeof(nus_log_buf));

static uint32_t nus_log_format_flags;

static void nus_backend_process(const struct log_backend *const backend,
                                union log_msg_generic *msg)
{
    ARG_UNUSED(backend);
    if (!current_conn) {
        return;
    }
    log_output_msg_process(&nus_log_output, &msg->log, nus_log_format_flags);
}

static void nus_backend_panic(const struct log_backend *const backend)
{
    ARG_UNUSED(backend);
    log_output_flush(&nus_log_output);
}

static int nus_backend_format_set(const struct log_backend *const backend,
                                  uint32_t log_type)
{
    ARG_UNUSED(backend);
    nus_log_format_flags = log_type;
    return 0;
}

static const struct log_backend_api nus_backend_api = {
    .process = nus_backend_process,
    .panic = nus_backend_panic,
    .format_set = nus_backend_format_set,
};

LOG_BACKEND_DEFINE(nus_log_backend, nus_backend_api, true);

int ble_transport_init(void)
{
    int err;

    err = bt_conn_auth_cb_register(&auth_cb);
    if (err) {
        printk("bt_conn_auth_cb_register failed: %d\n", err);
        return err;
    }

    err = bt_conn_auth_info_cb_register(&auth_info_cb);
    if (err) {
        printk("bt_conn_auth_info_cb_register failed: %d\n", err);
        return err;
    }

    err = bt_enable(NULL);
    if (err) {
        printk("bt_enable failed: %d\n", err);
        return err;
    }

    /* Load persisted BLE bond data from NVS (storage_partition). Without this
     * the LTK established during the previous pairing is forgotten on every
     * reboot, which forces macOS to re-pair every time and breaks reconnects
     * where macOS tries to use its cached LTK.
     */
    err = settings_load();
    if (err) {
        printk("settings_load failed: %d (continuing without persisted bonds)\n",
               err);
    }

    err = bt_nus_init(&nus_callbacks);
    if (err) {
        printk("bt_nus_init failed: %d\n", err);
        return err;
    }

    advertising_start();

    printk("BLE up: name='%s', fixed passkey=%06u, security required=L3\n",
           CONFIG_BT_DEVICE_NAME, FIXED_PASSKEY);
    return 0;
}
