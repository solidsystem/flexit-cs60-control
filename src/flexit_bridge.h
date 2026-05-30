#ifndef FLEXITMC_FLEXIT_BRIDGE_H_
#define FLEXITMC_FLEXIT_BRIDGE_H_

/* Phase 5 — glue between the RS485 panel mirror and the Zigbee data model.
 *
 * Replaces the Phase 2 synthetic feed in zigbee_ep.c with real CS60 data:
 *   - periodically snapshots panel_mirror and pushes supply/extract/outdoor
 *     temperatures and the current mode into the zigbee_ep setters;
 *   - registers the FanMode-write handler so a Zigbee client writing FanMode
 *     injects a real CMD_MODE via flexit_slave_queue_mode().
 *
 * Keeps zigbee_ep.c protocol-agnostic: it only knows its public setter API,
 * while this module owns the RS485-side wiring.
 */

/* Register the FanMode-write handler and start the periodic poll worker. Call
 * once at boot, after zigbee_ep_init(). Returns 0 on success.
 */
int flexit_bridge_init(void);

#endif /* FLEXITMC_FLEXIT_BRIDGE_H_ */
