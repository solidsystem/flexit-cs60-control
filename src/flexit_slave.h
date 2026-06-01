#ifndef FLEXITMC_FLEXIT_SLAVE_H_
#define FLEXITMC_FLEXIT_SLAVE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Modbus slave for the Flexit CL60/CS60 bus.
 *
 * Lives at slave address 1, owns a coil table, a holding-register table,
 * and an input-register table. To request a mode change we set both
 * coil[0] and reg[0]; the CS60 then issues an FC01 (sees the coil), an
 * FC03 (reads the value), and broadcasts FC65 (acks by clearing the coil
 * and stamping the reg) — at which point the new mode shows up in the
 * next FC10 status broadcast (already mirrored by panel_mirror).
 *
 * Address 1 is deliberate: the CS60 lets the *lowest* bus address own the
 * temperature setpoint, and the CI60 panel (with its setpoint potentiometer)
 * sits at addr 2. Sitting below it (addr 1) is what makes the CS60 accept our
 * setpoint writes rather than restamping the pot's value. See
 * flexit-cs60-communication.md.
 *
 * At boot the CS60 enumerates slaves by sending FC04 (Read Input Registers
 * regs 0..3) to addresses 0xFE, 0x01, 0x02, 0x03 in turn. Whoever answers
 * with a valid FC04 response gets registered as a polled slave. Our FC04
 * table mirrors CI60's reply ([0, 0, 1, 0x200]) so the CS60 treats us as
 * an additional panel.
 *
 * Two commands are emulated, both via the coil+register mechanism:
 *   - Slot 0  (`CMD_MODE`)     : coil 0  + reg 0x0000, temperature speed 0..3.
 *   - Slot 12 (`CMD_SETPOINT`) : coil 12 + reg 0x000C, temperature ×10 °C.
 * For both we set the reg, raise the coil, and self-clear the coil once the
 * CS60's FC03 read has been served (a single read delivers the value); the
 * CS60 also broadcasts an FC65 ack carrying the adopted value. The setpoint is
 * only accepted because we outrank the CI60 panel on the bus (see above).
 * Tables are sized for headroom (16 coils / 16 holding regs / 4 input regs)
 * but the matcher tolerates any addr/qty inside that range so we can grow
 * without breaking the wire format.
 */

/* Addr 1: below the CI60 panel (addr 2) so we own the setpoint (see above).
 * Changing this requires a CS60 power-cycle to re-enumerate us.
 */
#define FLEXIT_SLAVE_ADDR             1u
#define FLEXIT_SLAVE_COIL_COUNT       16u
#define FLEXIT_SLAVE_REG_COUNT        16u
#define FLEXIT_SLAVE_INPUT_REG_COUNT  4u

#define FLEXIT_SLAVE_CMD_MODE_ADDR     0x0000u

/* Setpoint command: coil 12 + holding register 0x000C, temperature ×10 °C.
 * (The CI60 panel drives the same coil/register from its potentiometer; we
 * outrank it at addr 1.) See flexit-cs60-communication.md.
 */
#define FLEXIT_SLAVE_CMD_SETPOINT_ADDR 0x000Cu

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

    /* Setpoint command (coil 12 / reg 0x000C). pending_setpoint is the last
     * value handed to flexit_slave_queue_setpoint() (°C ×10; 0xFFFF if none).
     */
    uint16_t pending_setpoint;
    bool     coil_setpoint_pending;
    uint16_t reg_setpoint_value;
    uint32_t setpoint_reads;    /* FC03 reads of reg 0x000C we served */
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

/* Queue a setpoint change. Stores the temperature (°C ×10) at reg 0x000C and
 * raises coil 12; the CS60's next FC01 poll sees the coil, the FC03 read of
 * reg 0x000C returns the value, and the CS60 adopts it (reflected in the FC10
 * status broadcast). There is no FC65 ack for the setpoint, so coil 12 is
 * self-cleared once the read has been served. Returns 0 (any int16 accepted;
 * the caller is expected to clamp to the unit's range).
 */
int flexit_slave_queue_setpoint(uint16_t value_x10);

/* Atomic copy of the current slave state into *out. */
void flexit_slave_snapshot(struct flexit_slave_snapshot *out);

#endif /* FLEXITMC_FLEXIT_SLAVE_H_ */
