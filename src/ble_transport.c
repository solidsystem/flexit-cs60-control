#include "ble_transport.h"
#include "flexit_slave.h"
#include "rs485_store.h"
#include "panel_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ---------------------------------------------------------------------------
 * RS485 live streaming
 *
 * When active, raw RS485 bytes are forwarded over NUS TX as they arrive
 * from the UART drain timer. The client enables streaming by sending the
 * "stream" command and disables it with "stop". Streaming is also cleared
 * automatically on disconnect.
 * ---------------------------------------------------------------------------
 */
static atomic_t stream_active = ATOMIC_INIT(0);

void ble_transport_forward_rs485(const uint8_t *data, size_t len)
{
    if (!atomic_get(&stream_active) || !current_conn || len == 0) {
        return;
    }

    /* bt_nus_send() requires the payload to fit within ATT_MTU - 3.
     * Query the negotiated MTU; fall back to 20 (BLE default) if not yet
     * exchanged. Remaining bytes are dropped on TX pool exhaustion —
     * the rs485_store ring always retains a copy for a later fetch.
     */
    uint16_t att_mtu   = bt_gatt_get_mtu(current_conn);
    uint16_t chunk_max = (att_mtu > 3u) ? (att_mtu - 3u) : 20u;

    size_t off = 0;
    while (off < len) {
        uint16_t chunk = (uint16_t)MIN(len - off, (size_t)chunk_max);
        if (bt_nus_send(current_conn, data + off, chunk) != 0) {
            break; /* TX pool full or disconnected — drop remainder */
        }
        off += chunk;
    }
}

/* ---------------------------------------------------------------------------
 * One-shot snapshot fetch over NUS
 *
 * Protocol (device → client via NUS TX notifications):
 *   Byte 0-3   : total payload length N, little-endian uint32
 *   Byte 4-N+3 : raw RS485 bytes, oldest first
 *
 * State machine uses fetch_pos == FETCH_HDR_PENDING to mean "header not
 * yet sent".
 * ---------------------------------------------------------------------------
 */
#define FETCH_CHUNK_SIZE  200u
#define FETCH_HDR_PENDING UINT32_MAX

static uint8_t  fetch_buf[RS485_STORE_SIZE];
static uint32_t fetch_total;
static uint32_t fetch_pos;

static void fetch_send_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(fetch_send_work, fetch_send_work_handler);

static void fetch_send_work_handler(struct k_work *work)
{
    if (!current_conn) {
        return;
    }

    int err;

    if (fetch_pos == FETCH_HDR_PENDING) {
        uint8_t hdr[4] = {
            (uint8_t)(fetch_total),
            (uint8_t)(fetch_total >> 8),
            (uint8_t)(fetch_total >> 16),
            (uint8_t)(fetch_total >> 24),
        };
        err = bt_nus_send(current_conn, hdr, sizeof(hdr));
        if (err == -ENOMEM) {
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(20));
            return;
        }
        if (err != 0) {
            printk("fetch: header send error %d\n", err);
            return;
        }
        fetch_pos = 0;
    }

    if (fetch_pos < fetch_total) {
        uint32_t rem   = fetch_total - fetch_pos;
        uint32_t chunk = MIN(rem, FETCH_CHUNK_SIZE);

        err = bt_nus_send(current_conn, fetch_buf + fetch_pos, chunk);
        if (err == -ENOMEM) {
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(20));
            return;
        }
        if (err != 0) {
            printk("fetch: data send error %d at offset %u\n", err, fetch_pos);
            return;
        }
        fetch_pos += chunk;

        if (fetch_pos < fetch_total) {
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(10));
        } else {
            printk("fetch: sent %u bytes\n", fetch_total);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Advertising helpers
 * ---------------------------------------------------------------------------
 */
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

static void adv_restart_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (current_conn == NULL) {
        advertising_start();
    }
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

/* ---------------------------------------------------------------------------
 * Connection callbacks
 * ---------------------------------------------------------------------------
 */
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

    atomic_set(&stream_active, 0);
    k_work_cancel_delayable(&fetch_send_work);

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
    .connected        = connected,
    .disconnected     = disconnected,
    .security_changed = security_changed,
};

/* ---------------------------------------------------------------------------
 * Pairing / auth callbacks
 * ---------------------------------------------------------------------------
 */
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
    .cancel      = auth_cancel,
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
    .pairing_failed   = pairing_failed,
};

/* ---------------------------------------------------------------------------
 * `state` command — render the panel-mirror snapshot
 *
 * Single-line ASCII, key=value pairs separated by spaces, terminated by '\n'.
 * Temperatures are formatted as `N.D` (one decimal, ×10 raw); sensors that
 * are not present surface as "n/a". The line is appended with one
 * `CNT[0xAAAA]=VVVV@Ts` slot per observed FC06 counter. The whole thing
 * fits in one ATT MTU (498-3 = 495 bytes; the line tops out around ~360).
 * ---------------------------------------------------------------------------
 */
static void format_temp_x10(char *out, size_t cap, int16_t x10, bool present)
{
    if (!present) {
        snprintf(out, cap, "n/a");
        return;
    }
    if (x10 < 0) {
        snprintf(out, cap, "-%d.%d", (-x10) / 10, (-x10) % 10);
    } else {
        snprintf(out, cap, "%d.%d", x10 / 10, x10 % 10);
    }
}

static size_t format_state_line(char *buf, size_t cap)
{
    struct panel_mirror_state s;
    panel_mirror_snapshot(&s);

    char t_set[16], t_set2[16], t_sup[16], t_ext[16], t_out[16], t_ret[16];
    format_temp_x10(t_set,  sizeof(t_set),  s.temp_setpoint_x10,     true);
    format_temp_x10(t_set2, sizeof(t_set2), s.temp_setpoint_2_x10,   true);
    format_temp_x10(t_sup,  sizeof(t_sup),  s.temp_supply_air_x10,   true);
    format_temp_x10(t_out,  sizeof(t_out),  s.temp_outdoor_air_x10,  true);
    format_temp_x10(t_ext,  sizeof(t_ext),  s.temp_extract_air_x10,
                    (s.sensors_present & PANEL_MIRROR_SENSOR_EXTRACT_AIR) != 0);
    format_temp_x10(t_ret,  sizeof(t_ret),  s.temp_return_water_x10,
                    (s.sensors_present & PANEL_MIRROR_SENSOR_RETURN_WATER) != 0);

    int w = snprintf(buf, cap,
        "MODE=%u SET=%s SET2=%s "
        "T_SUP=%s T_EXT=%s T_OUT=%s T_RET=%s "
        "PCT_COOL=%u PCT_HX=%u PCT_HEAT=%u PCT_FAN=%u "
        "UNK1=0x%04X UNK2=0x%04X "
        "FC10=%u FC06=%u CRCERR=%u LAST_MS=%llu",
        s.mode, t_set, t_set2,
        t_sup, t_ext, t_out, t_ret,
        s.pct_cooling, s.pct_heat_exchanger, s.pct_heating, s.pct_supply_fan,
        s.unknown_1, s.unknown_2,
        s.fc10_frames, s.fc06_frames, s.crc_failures,
        (unsigned long long)s.last_fc10_uptime_ms);

    if (w < 0) {
        return 0;
    }
    size_t pos = ((size_t)w < cap) ? (size_t)w : cap - 1;

    /* Append slave-side cycle state so the client can watch the mode
     * change progress without a separate command. MODE_QUEUED is what the
     * BLE side asked for; MODE_ACKED reflects the FC65 that closes the
     * cycle. Either may be "none" before the first command of the boot.
     */
    struct flexit_slave_snapshot sl;
    flexit_slave_snapshot(&sl);
    if (pos < cap - 1) {
        if (sl.pending_mode == 0xFFFFu) {
            w = snprintf(buf + pos, cap - pos, " MODE_QUEUED=none");
        } else {
            w = snprintf(buf + pos, cap - pos, " MODE_QUEUED=%u",
                         sl.pending_mode);
        }
        if (w > 0 && (size_t)w < cap - pos) {
            pos += (size_t)w;
        }
    }
    if (pos < cap - 1) {
        if (sl.last_acked_mode == 0xFFFFu) {
            w = snprintf(buf + pos, cap - pos, " MODE_ACKED=none");
        } else {
            w = snprintf(buf + pos, cap - pos,
                         " MODE_ACKED=%u@%llu",
                         sl.last_acked_mode,
                         (unsigned long long)sl.last_ack_uptime_ms);
        }
        if (w > 0 && (size_t)w < cap - pos) {
            pos += (size_t)w;
        }
    }
    if (pos < cap - 1) {
        w = snprintf(buf + pos, cap - pos,
                     " SLV_FC01=%u SLV_FC03=%u SLV_FC04=%u SLV_FC65=%u SLV_COIL0=%u",
                     sl.fc01_serviced, sl.fc03_serviced, sl.fc04_serviced,
                     sl.fc65_acks, sl.coil0_pending ? 1u : 0u);
        if (w > 0 && (size_t)w < cap - pos) {
            pos += (size_t)w;
        }
    }

    uint64_t now = (uint64_t)k_uptime_get();
    for (int i = 0; i < PANEL_MIRROR_COUNTER_SLOTS && pos < cap - 1; i++) {
        if (!s.counters[i].used) {
            continue;
        }
        uint64_t age_s = (now - s.counters[i].at_ms) / 1000u;
        w = snprintf(buf + pos, cap - pos,
                     " CNT[0x%04X]=%u@%llus",
                     s.counters[i].addr, s.counters[i].value,
                     (unsigned long long)age_s);
        if (w < 0) {
            break;
        }
        if ((size_t)w >= cap - pos) {
            pos = cap - 1;
            break;
        }
        pos += (size_t)w;
    }

    if (pos < cap - 1) {
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return pos;
}

/* ---------------------------------------------------------------------------
 * NUS receive — command dispatch
 *
 * Commands (sent by ble-client via NUS RX write):
 *   "fetch"   — take a one-shot snapshot of rs485_store and send it
 *   "stream"  — start forwarding RS485 bytes in real time over NUS TX
 *   "stop"    — stop streaming
 *   "state"   — single-line snapshot of the decoded panel mirror
 *   "mode N"  — queue a CMD_MODE change (N = 0..3) on the Modbus slave;
 *               echoes "mode: queued=N" or "mode: bad arg" on NUS TX
 * ---------------------------------------------------------------------------
 */
static void nus_received(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    printk("NUS RX: %.*s\n", len, (const char *)data);

    if (len >= 5 && memcmp(data, "fetch", 5) == 0) {
        printk("fetch: snapshotting RS485 store\n");
        fetch_total = (uint32_t)rs485_store_snapshot(fetch_buf, sizeof(fetch_buf));
        fetch_pos   = FETCH_HDR_PENDING;
        k_work_schedule(&fetch_send_work, K_NO_WAIT);

    } else if (len >= 6 && memcmp(data, "stream", 6) == 0) {
        printk("stream: started\n");
        atomic_set(&stream_active, 1);

    } else if (len >= 5 && memcmp(data, "state", 5) == 0) {
        char   line[512];
        size_t n = format_state_line(line, sizeof(line));
        int    err = bt_nus_send(conn, (const uint8_t *)line, (uint16_t)n);
        if (err) {
            printk("state: bt_nus_send failed: %d\n", err);
        }

    } else if (len >= 4 && memcmp(data, "stop", 4) == 0) {
        printk("stream: stopped\n");
        atomic_set(&stream_active, 0);

    } else if (len >= 4 && memcmp(data, "mode", 4) == 0) {
        /* Parse the rest of the payload as a decimal integer 0..3. */
        char    buf[16];
        char    reply[40];
        size_t  copy = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, data, copy);
        buf[copy] = '\0';

        char *p = buf + 4;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        int n_reply;
        if (*p < '0' || *p > '9') {
            n_reply = snprintf(reply, sizeof(reply),
                               "mode: bad arg\n");
        } else {
            int mode = atoi(p);
            int err  = flexit_slave_queue_mode((uint16_t)mode);
            if (err == 0) {
                n_reply = snprintf(reply, sizeof(reply),
                                   "mode: queued=%d\n", mode);
            } else {
                n_reply = snprintf(reply, sizeof(reply),
                                   "mode: bad arg\n");
            }
        }
        if (n_reply > 0) {
            (void)bt_nus_send(conn, (const uint8_t *)reply,
                              (uint16_t)n_reply);
        }
    }
}

static struct bt_nus_cb nus_callbacks = {
    .received = nus_received,
};

/* ---------------------------------------------------------------------------
 * Public init
 * ---------------------------------------------------------------------------
 */
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
