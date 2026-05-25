#include "ble_transport.h"
#include "rs485_store.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/settings/settings.h>

#include <bluetooth/services/nus.h>

#define FIXED_PASSKEY 444999u

static struct bt_conn *current_conn;

/* Dump-on-subscribe machinery.
 *
 * When the central enables notifications on the NUS TX characteristic, the
 * NUS service's `send_enabled` callback fires. We hand the connection off to
 * a dedicated dump thread which: (1) snapshots the RS485 ring buffer,
 * (2) streams every stored frame back as space-separated hex over NUS
 * notifications, (3) disconnects. Running this on its own thread keeps the
 * BT RX thread responsive and lets the RS485 flush_work in main.c continue
 * appending new frames while the dump is in flight.
 */
static K_THREAD_STACK_DEFINE(dump_thread_stack, 2048);
static struct k_thread dump_thread_data;
static struct k_sem dump_sem;
static struct bt_conn *dump_conn;
static atomic_t dump_in_progress = ATOMIC_INIT(0);

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

/* Fired by the NUS service when the central subscribes to (or unsubscribes
 * from) notifications on the NUS TX characteristic. We use the ENABLED edge
 * as the trigger to dump the stored RS485 ring buffer and then close the
 * link.
 */
static void nus_send_enabled(enum bt_nus_send_status status)
{
    if (status != BT_NUS_SEND_STATUS_ENABLED) {
        return;
    }
    if (!current_conn) {
        return;
    }
    /* atomic_cas returns true iff dump_in_progress transitioned 0 -> 1.
     * Without this guard a stray "subscribe" event during an in-flight dump
     * would leak the previous bt_conn_ref.
     */
    if (!atomic_cas(&dump_in_progress, 0, 1)) {
        return;
    }
    dump_conn = bt_conn_ref(current_conn);
    k_sem_give(&dump_sem);
}

static struct bt_nus_cb nus_callbacks = {
    .received = nus_received,
    .send_enabled = nus_send_enabled,
};

/* ---- Dump thread: drains the RS485 ring out over NUS, then disconnects. ----
 *
 * Notification chunk size is capped well below the negotiated MTU so each
 * line of formatted hex output fits in one notification; bt_nus_send returns
 * -ENOMEM when the GATT TX pool is momentarily full, in which case we sleep
 * briefly and retry.
 */

#define NUS_CHUNK_BYTES 200

static int nus_send_blocking(struct bt_conn *conn,
                             const uint8_t *data, uint16_t len)
{
    for (;;) {
        int err = bt_nus_send(conn, data, len);
        if (err == 0) {
            return 0;
        }
        if (err == -ENOMEM) {
            k_sleep(K_MSEC(20));
            continue;
        }
        /* -ENOTCONN, -EINVAL, ... – not worth retrying. */
        return err;
    }
}

static int nus_send_chunked(struct bt_conn *conn,
                            const char *data, size_t len)
{
    while (len > 0) {
        size_t n = (len > NUS_CHUNK_BYTES) ? NUS_CHUNK_BYTES : len;
        int err = nus_send_blocking(conn, (const uint8_t *)data, (uint16_t)n);
        if (err) {
            return err;
        }
        data += n;
        len -= n;
    }
    return 0;
}

static void do_dump(struct bt_conn *conn)
{
    static uint8_t snap[RS485_STORE_SIZE];
    size_t n = rs485_store_snapshot(snap, sizeof(snap));

    if (n == 0) {
        static const char empty[] = "(no RS485 frames stored)\n";
        (void)nus_send_chunked(conn, empty, sizeof(empty) - 1);
        return;
    }

    /* Worst-case formatted line for a single 64 KiB-1 frame would be huge,
     * but in practice RS485 frames here top out around 256 B. The buffer
     * below comfortably fits any frame we'd ever store.
     */
    static char line[16 + 256 * 3 + 4];

    size_t i = 0;
    while (i + 2 <= n) {
        uint16_t flen = (uint16_t)snap[i] | ((uint16_t)snap[i + 1] << 8);
        i += 2;
        if (i + flen > n) {
            break;
        }

        int p = snprintk(line, sizeof(line), "RS485 RX (%u):", flen);
        for (uint16_t j = 0; j < flen && p < (int)sizeof(line) - 4; j++) {
            p += snprintk(line + p, sizeof(line) - p, " %02X", snap[i + j]);
        }
        if (p < (int)sizeof(line) - 1) {
            line[p++] = '\n';
        }
        if (nus_send_chunked(conn, line, (size_t)p) != 0) {
            return;
        }

        i += flen;
    }
}

static void dump_thread_entry(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        k_sem_take(&dump_sem, K_FOREVER);

        struct bt_conn *conn = dump_conn;
        if (conn) {
            do_dump(conn);
            /* Give the controller a brief moment to push the last
             * notifications onto the air before we drop the link.
             */
            k_sleep(K_MSEC(100));
            (void)bt_conn_disconnect(conn,
                                     BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(conn);
            dump_conn = NULL;
        }
        atomic_set(&dump_in_progress, 0);
    }
}

int ble_transport_init(void)
{
    int err;

    k_sem_init(&dump_sem, 0, 1);
    k_thread_create(&dump_thread_data, dump_thread_stack,
                    K_THREAD_STACK_SIZEOF(dump_thread_stack),
                    dump_thread_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    k_thread_name_set(&dump_thread_data, "rs485_dump");

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
