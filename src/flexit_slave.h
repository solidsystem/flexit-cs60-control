#ifndef FLEXITMC_FLEXIT_SLAVE_H_
#define FLEXITMC_FLEXIT_SLAVE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Phase 2 — Modbus slave for the Flexit CL60/CS60 bus.
 *
 * Lives at slave address 3, owns a coil table, a holding-register table,
 * and an input-register table. To request a mode change we set both
 * coil[0] and reg[0]; the CS60 then issues an FC01 (sees the coil), an
 * FC03 (reads the value), and broadcasts FC65 (acks by clearing the coil
 * and stamping the reg) — at which point the new mode shows up in the
 * next FC10 status broadcast (already mirrored by panel_mirror).
 *
 * At boot the CS60 enumerates slaves by sending FC04 (Read Input Registers
 * regs 0..3) to addresses 0xFE, 0x01, 0x02, 0x03 in turn. Whoever answers
 * with a valid FC04 response gets registered as a polled slave. Our FC04
 * table mirrors CI60's reply ([0, 0, 1, 0x200]) so the CS60 treats us as
 * an additional panel.
 *
 * Slot 0 (`CMD_MODE`) is currently the only holding reg we ever raise.
 * Tables are sized for headroom (16 coils / 16 holding regs / 4 input regs)
 * but the matcher tolerates any addr/qty inside that range so we can grow
 * without breaking the wire format.
 */

#define FLEXIT_SLAVE_ADDR             3u
#define FLEXIT_SLAVE_COIL_COUNT       16u
#define FLEXIT_SLAVE_REG_COUNT        16u
#define FLEXIT_SLAVE_INPUT_REG_COUNT  4u

#define FLEXIT_SLAVE_CMD_MODE_ADDR 0x0000u

struct flexit_slave_snapshot {
    /* Last value handed to flexit_slave_queue_mode(); 0xFFFF if none yet. */
    uint16_t pending_mode;

    /* Last value the CS60 acked via FC65 at addr 0; 0xFFFF if none yet. */
    uint16_t last_acked_mode;

    /* Uptime (ms) of the most recent FC65 ack for addr 0. */
    uint64_t last_ack_uptime_ms;

    /* Diagnostic counters. */
    uint32_t fc01_serviced;     /* FC01 requests we answered */
    uint32_t fc03_serviced;     /* FC03 requests we answered */
    uint32_t fc04_serviced;     /* FC04 enumeration probes we answered */
    uint32_t fc65_acks;         /* FC65 broadcasts processed (any addr) */
    uint32_t tx_errors;         /* rs485_uart_send() failures */
    uint32_t crc_failures;      /* candidate frames that failed CRC */

    /* Current coil 0 / reg 0 (pending state visible on the wire). */
    bool     coil0_pending;
    uint16_t reg0_value;
};

/* Reset internal state. Idempotent — safe to call once at boot. */
int flexit_slave_init(void);

/* Feed raw RS485 bytes (same stream that goes to panel_mirror_feed). The
 * slave maintains its own accumulator. Thread-safe; intended to be called
 * from the UART drain worker.
 */
void flexit_slave_feed(const uint8_t *data, size_t len);

/* Queue a CMD_MODE change. Stores the value at reg 0 and raises coil 0; the
 * next FC01 poll by the CS60 will see the coil set, the matching FC03 read
 * will return the value, and the resulting FC65 broadcast will clear the
 * coil. Returns -EINVAL for out-of-range mode values (must be 0..3).
 */
int flexit_slave_queue_mode(uint16_t mode);

/* Atomic copy of the current slave state into *out. */
void flexit_slave_snapshot(struct flexit_slave_snapshot *out);

#endif /* FLEXITMC_FLEXIT_SLAVE_H_ */
