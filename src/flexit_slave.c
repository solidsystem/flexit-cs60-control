#include "flexit_slave.h"
#include "rs485_uart.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* Frame anchors we look for in the byte stream:
 *
 *   [03 01] FC01 ReadCoils request to us         — 8 B
 *   [03 03] FC03 ReadHoldingRegs request to us   — 8 B
 *   [03 04] FC04 ReadInputRegs probe to us       — 8 B
 *   [00 65] FC65 broadcast (coil/reg ack)        — 8 B
 *
 * All four are exactly 8 bytes on the wire. We anchor on the (addr, fc)
 * pair and validate CRC at offset 6; on CRC failure we slip one byte and
 * re-scan, mirroring panel_mirror.c.
 *
 * Response frames go out via rs485_uart_send(). The longest response we
 * emit is an FC01 reply covering the larger of the two CS60 poll spans
 * (352 coils → 44 byte_count → 49 B frame); 64 B leaves comfortable
 * headroom.
 */
#define FC_REQ_LEN      8u
#define ACCUM_SIZE      64u
#define RESP_BUF_SIZE   64u
#define COIL_TABLE_BITS (FLEXIT_SLAVE_COIL_COUNT)

#define SENTINEL_NONE 0xFFFFu

/* Input registers (FC04). The CS60 enumerates slaves at boot by reading
 * regs 0..3 from candidate addresses; CI60 answers with these exact four
 * values, so we mirror them to be treated as a registered panel.
 */
static const uint16_t input_regs[FLEXIT_SLAVE_INPUT_REG_COUNT] = {
    0x0000u, 0x0000u, 0x0001u, 0x0200u,
};

static struct {
    uint8_t  coils[(COIL_TABLE_BITS + 7) / 8];  /* bit 0 = coil 0 */
    uint16_t regs[FLEXIT_SLAVE_REG_COUNT];

    uint16_t pending_mode;
    uint16_t last_acked_mode;
    uint64_t last_ack_uptime_ms;

    uint32_t fc01_serviced;
    uint32_t fc03_serviced;
    uint32_t fc04_serviced;
    uint32_t fc65_acks;
    uint32_t tx_errors;
    uint32_t crc_failures;
} state;

static K_MUTEX_DEFINE(state_mutex);

static uint8_t accum[ACCUM_SIZE];
static size_t  accum_len;

static inline uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return ((uint16_t)p[1] << 8) | (uint16_t)p[0];
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void bump_crc_failure(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state.crc_failures++;
    k_mutex_unlock(&state_mutex);
}

/* ---------------------------------------------------------------------------
 * Response builders — produce a CRC-stamped frame ready to hand to rs485_uart.
 * Return total frame length on success, negative on parameter error.
 * ---------------------------------------------------------------------------
 */
static int build_fc01_response(uint8_t *resp, size_t cap,
                               uint16_t start, uint16_t qty)
{
    if (qty == 0 || qty > 2000) {
        return -EINVAL;
    }
    uint16_t bc = (uint16_t)((qty + 7) / 8);
    size_t   frame_len = (size_t)3 + bc + 2;
    if (frame_len > cap) {
        return -EINVAL;
    }

    resp[0] = FLEXIT_SLAVE_ADDR;
    resp[1] = 0x01;
    resp[2] = (uint8_t)bc;
    memset(resp + 3, 0, bc);

    /* Pack coil bits LSB-first within each byte, starting at `start`. */
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t coil_idx = start + i;
        if (coil_idx >= COIL_TABLE_BITS) {
            break; /* remaining bytes already zero-filled */
        }
        if (state.coils[coil_idx >> 3] & (uint8_t)(1u << (coil_idx & 7))) {
            resp[3 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
        }
    }

    uint16_t crc = crc16_ansi(resp, 3u + bc);
    put_le16(resp + 3 + bc, crc);
    return (int)frame_len;
}

static int build_fc03_response(uint8_t *resp, size_t cap,
                               uint16_t addr, uint16_t qty)
{
    if (qty == 0 || qty > 8) {
        return -EINVAL;
    }
    if ((size_t)addr + qty > FLEXIT_SLAVE_REG_COUNT) {
        return -EINVAL;
    }
    uint16_t bc = (uint16_t)(qty * 2);
    size_t   frame_len = (size_t)3 + bc + 2;
    if (frame_len > cap) {
        return -EINVAL;
    }

    resp[0] = FLEXIT_SLAVE_ADDR;
    resp[1] = 0x03;
    resp[2] = (uint8_t)bc;
    for (uint16_t i = 0; i < qty; i++) {
        put_be16(resp + 3 + i * 2, state.regs[addr + i]);
    }

    uint16_t crc = crc16_ansi(resp, 3u + bc);
    put_le16(resp + 3 + bc, crc);
    return (int)frame_len;
}

static int build_fc04_response(uint8_t *resp, size_t cap,
                               uint16_t addr, uint16_t qty)
{
    if (qty == 0 || qty > FLEXIT_SLAVE_INPUT_REG_COUNT) {
        return -EINVAL;
    }
    if ((size_t)addr + qty > FLEXIT_SLAVE_INPUT_REG_COUNT) {
        return -EINVAL;
    }
    uint16_t bc = (uint16_t)(qty * 2);
    size_t   frame_len = (size_t)3 + bc + 2;
    if (frame_len > cap) {
        return -EINVAL;
    }

    resp[0] = FLEXIT_SLAVE_ADDR;
    resp[1] = 0x04;
    resp[2] = (uint8_t)bc;
    for (uint16_t i = 0; i < qty; i++) {
        put_be16(resp + 3 + i * 2, input_regs[addr + i]);
    }

    uint16_t crc = crc16_ansi(resp, 3u + bc);
    put_le16(resp + 3 + bc, crc);
    return (int)frame_len;
}

/* ---------------------------------------------------------------------------
 * Request handlers — invoked when a candidate frame has passed CRC.
 * ---------------------------------------------------------------------------
 */
static void handle_fc01_request(const uint8_t *frame)
{
    uint16_t start = get_be16(frame + 2);
    uint16_t qty   = get_be16(frame + 4);

    uint8_t resp[RESP_BUF_SIZE];
    int     resp_len;

    k_mutex_lock(&state_mutex, K_FOREVER);
    resp_len = build_fc01_response(resp, sizeof(resp), start, qty);
    if (resp_len > 0) {
        state.fc01_serviced++;
    }
    k_mutex_unlock(&state_mutex);

    if (resp_len <= 0) {
        return; /* malformed request — stay silent */
    }

    int err = rs485_uart_send(resp, (size_t)resp_len);
    if (err != 0) {
        printk("flexit_slave: fc01 tx error %d\n", err);
        k_mutex_lock(&state_mutex, K_FOREVER);
        state.tx_errors++;
        k_mutex_unlock(&state_mutex);
    }
}

static void handle_fc03_request(const uint8_t *frame)
{
    uint16_t addr = get_be16(frame + 2);
    uint16_t qty  = get_be16(frame + 4);

    uint8_t resp[RESP_BUF_SIZE];
    int     resp_len;

    k_mutex_lock(&state_mutex, K_FOREVER);
    resp_len = build_fc03_response(resp, sizeof(resp), addr, qty);
    if (resp_len > 0) {
        state.fc03_serviced++;
    }
    k_mutex_unlock(&state_mutex);

    if (resp_len <= 0) {
        return;
    }

    int err = rs485_uart_send(resp, (size_t)resp_len);
    if (err != 0) {
        printk("flexit_slave: fc03 tx error %d\n", err);
        k_mutex_lock(&state_mutex, K_FOREVER);
        state.tx_errors++;
        k_mutex_unlock(&state_mutex);
    }
}

static void handle_fc04_request(const uint8_t *frame)
{
    uint16_t addr = get_be16(frame + 2);
    uint16_t qty  = get_be16(frame + 4);

    uint8_t resp[RESP_BUF_SIZE];
    int     resp_len;

    k_mutex_lock(&state_mutex, K_FOREVER);
    resp_len = build_fc04_response(resp, sizeof(resp), addr, qty);
    if (resp_len > 0) {
        state.fc04_serviced++;
    }
    k_mutex_unlock(&state_mutex);

    if (resp_len <= 0) {
        return;
    }

    int err = rs485_uart_send(resp, (size_t)resp_len);
    if (err != 0) {
        printk("flexit_slave: fc04 tx error %d\n", err);
        k_mutex_lock(&state_mutex, K_FOREVER);
        state.tx_errors++;
        k_mutex_unlock(&state_mutex);
    }
}

static void handle_fc65_broadcast(const uint8_t *frame)
{
    uint16_t addr  = get_be16(frame + 2);
    uint16_t value = get_be16(frame + 4);

    k_mutex_lock(&state_mutex, K_FOREVER);
    state.fc65_acks++;
    if (addr < FLEXIT_SLAVE_REG_COUNT) {
        state.regs[addr] = value;
        if (addr < COIL_TABLE_BITS) {
            state.coils[addr >> 3] &= (uint8_t)~(1u << (addr & 7));
        }
        if (addr == FLEXIT_SLAVE_CMD_MODE_ADDR) {
            state.last_acked_mode    = value;
            state.last_ack_uptime_ms = (uint64_t)k_uptime_get();
        }
    }
    k_mutex_unlock(&state_mutex);
}

/* ---------------------------------------------------------------------------
 * Frame matcher — try to decode one frame anchored at accum[0].
 * Returns bytes consumed, or 0 if more bytes are needed.
 * ---------------------------------------------------------------------------
 */
static bool crc_ok(const uint8_t *frame, size_t len)
{
    uint16_t calc = crc16_ansi(frame, len - 2);
    uint16_t recv = get_le16(frame + len - 2);
    return calc == recv;
}

static size_t try_decode_at_front(void)
{
    if (accum_len < 2) {
        return 0;
    }

    uint8_t a = accum[0];
    uint8_t b = accum[1];

    /* FC01 request to us. */
    if (a == FLEXIT_SLAVE_ADDR && b == 0x01) {
        if (accum_len < FC_REQ_LEN) {
            return 0;
        }
        if (crc_ok(accum, FC_REQ_LEN)) {
            handle_fc01_request(accum);
            return FC_REQ_LEN;
        }
        bump_crc_failure();
        return 1;
    }

    /* FC03 request to us. */
    if (a == FLEXIT_SLAVE_ADDR && b == 0x03) {
        if (accum_len < FC_REQ_LEN) {
            return 0;
        }
        if (crc_ok(accum, FC_REQ_LEN)) {
            handle_fc03_request(accum);
            return FC_REQ_LEN;
        }
        bump_crc_failure();
        return 1;
    }

    /* FC04 enumeration probe to us. */
    if (a == FLEXIT_SLAVE_ADDR && b == 0x04) {
        if (accum_len < FC_REQ_LEN) {
            return 0;
        }
        if (crc_ok(accum, FC_REQ_LEN)) {
            handle_fc04_request(accum);
            return FC_REQ_LEN;
        }
        bump_crc_failure();
        return 1;
    }

    /* FC65 broadcast. */
    if (a == 0x00 && b == 0x65) {
        if (accum_len < FC_REQ_LEN) {
            return 0;
        }
        if (crc_ok(accum, FC_REQ_LEN)) {
            handle_fc65_broadcast(accum);
            return FC_REQ_LEN;
        }
        bump_crc_failure();
        return 1;
    }

    /* Anything else — slip one byte. */
    return 1;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */
/* Default value pre-armed at boot. Matches the panel's current operating
 * mode (Normal) so that — if the CS60 happens to poll us during its own
 * boot enumeration and runs the full coil/FC03/FC65 cycle — the resulting
 * FC65 write is a no-op against the present state.
 *
 * Rationale: when the user power-cycles the CS60 the XIAO loses power too
 * (shared RJ12 supply), so any "pending" command in RAM is wiped. By
 * pre-arming coil 0 we give the freshly-booted CS60 something to discover
 * the very first time it might probe slave addresses.
 */
#define FLEXIT_SLAVE_BOOT_MODE 2u

int flexit_slave_init(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    memset(&state, 0, sizeof(state));
    state.regs[FLEXIT_SLAVE_CMD_MODE_ADDR]        = FLEXIT_SLAVE_BOOT_MODE;
    state.coils[FLEXIT_SLAVE_CMD_MODE_ADDR >> 3] |=
        (uint8_t)(1u << (FLEXIT_SLAVE_CMD_MODE_ADDR & 7));
    state.pending_mode    = FLEXIT_SLAVE_BOOT_MODE;
    state.last_acked_mode = SENTINEL_NONE;
    k_mutex_unlock(&state_mutex);
    accum_len = 0;
    printk("flexit_slave: addr=%u, %u coils / %u regs, boot-armed mode=%u\n",
           FLEXIT_SLAVE_ADDR, FLEXIT_SLAVE_COIL_COUNT, FLEXIT_SLAVE_REG_COUNT,
           FLEXIT_SLAVE_BOOT_MODE);
    return 0;
}

void flexit_slave_feed(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    if (len >= ACCUM_SIZE) {
        memcpy(accum, data + (len - ACCUM_SIZE), ACCUM_SIZE);
        accum_len = ACCUM_SIZE;
    } else {
        if (accum_len + len > ACCUM_SIZE) {
            size_t drop = (accum_len + len) - ACCUM_SIZE;
            memmove(accum, accum + drop, accum_len - drop);
            accum_len -= drop;
        }
        memcpy(accum + accum_len, data, len);
        accum_len += len;
    }

    while (accum_len > 0) {
        size_t consumed = try_decode_at_front();
        if (consumed == 0) {
            break;
        }
        if (consumed < accum_len) {
            memmove(accum, accum + consumed, accum_len - consumed);
        }
        accum_len -= consumed;
    }
}

int flexit_slave_queue_mode(uint16_t mode)
{
    if (mode > 3u) {
        return -EINVAL;
    }
    k_mutex_lock(&state_mutex, K_FOREVER);
    state.regs[FLEXIT_SLAVE_CMD_MODE_ADDR] = mode;
    state.coils[FLEXIT_SLAVE_CMD_MODE_ADDR >> 3] |=
        (uint8_t)(1u << (FLEXIT_SLAVE_CMD_MODE_ADDR & 7));
    state.pending_mode = mode;
    k_mutex_unlock(&state_mutex);
    printk("flexit_slave: queued mode=%u (coil 0 raised, reg 0 = %u)\n",
           mode, mode);
    return 0;
}

void flexit_slave_snapshot(struct flexit_slave_snapshot *out)
{
    if (out == NULL) {
        return;
    }
    k_mutex_lock(&state_mutex, K_FOREVER);
    out->pending_mode        = state.pending_mode;
    out->last_acked_mode     = state.last_acked_mode;
    out->last_ack_uptime_ms  = state.last_ack_uptime_ms;
    out->fc01_serviced       = state.fc01_serviced;
    out->fc03_serviced       = state.fc03_serviced;
    out->fc04_serviced       = state.fc04_serviced;
    out->fc65_acks           = state.fc65_acks;
    out->tx_errors           = state.tx_errors;
    out->crc_failures        = state.crc_failures;
    out->coil0_pending       = (state.coils[0] & 0x01u) != 0;
    out->reg0_value          = state.regs[FLEXIT_SLAVE_CMD_MODE_ADDR];
    k_mutex_unlock(&state_mutex);
}
