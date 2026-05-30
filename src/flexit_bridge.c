/* Phase 5 — bridge the RS485 panel mirror to the Zigbee data model.
 *
 * RS485 (CS60) ── panel_mirror ──┐
 *                                 ├─► zigbee_ep setters (temps + mode)
 * ZHA FanMode write ── zigbee_ep ─┴─► flexit_slave_queue_mode (CMD_MODE)
 *
 * The poll worker runs on the system workqueue: it snapshots the mirror and
 * latches the values into zigbee_ep, which flushes them into the ZCL
 * attributes from the ZBOSS thread on its own publish tick. See
 * smarthouse-integration.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "panel_mirror.h"
#include "flexit_slave.h"
#include "zigbee_ep.h"
#include "flexit_bridge.h"

LOG_MODULE_REGISTER(flexit_bridge, LOG_LEVEL_INF);

/* How often to copy the mirror into the Zigbee setters. Faster than the
 * zigbee_ep publish tick is pointless (the setters just latch); 2 s keeps the
 * latched values fresh well inside FLEXIT_TEMP_STALE_MS without busy-polling.
 */
#define BRIDGE_POLL_INTERVAL_S 2

/* FanMode write (ZHA → CS60): inject the real CMD_MODE the same way
 * `ble-client mode N` does. flexit_mode is already 0..3 (validated upstream).
 */
static void mode_write_handler(uint8_t flexit_mode)
{
	int err = flexit_slave_queue_mode(flexit_mode);
	if (err) {
		LOG_WRN("flexit_slave_queue_mode(%u) failed: %d", flexit_mode, err);
	}
}

static void bridge_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(bridge_work, bridge_work_fn);

static void bridge_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct panel_mirror_state st;
	panel_mirror_snapshot(&st);

	/* Nothing decoded yet (mirror still zero-initialised) — don't push a
	 * spurious 0.00 C / Stop; leave the channels at "unknown" until the
	 * first FC10 broadcast lands.
	 */
	if (st.fc10_frames > 0) {
		/* Panel temps are int16 ×10 °C; ZCL MeasuredValue is ×100
		 * (centi-°C). Range -40..80 °C keeps ×100 inside int16.
		 */
		zigbee_ep_set_temperature(ZIGBEE_TEMP_SUPPLY,
			(int16_t)(st.temp_supply_air_x10 * 10));
		zigbee_ep_set_temperature(ZIGBEE_TEMP_OUTDOOR,
			(int16_t)(st.temp_outdoor_air_x10 * 10));

		/* Extract-air sensor may be absent on this install (sentinel on
		 * the wire). Skip it when not present so zigbee_ep ages it out to
		 * "unknown" rather than publishing the sentinel as a reading.
		 */
		if (st.sensors_present & PANEL_MIRROR_SENSOR_EXTRACT_AIR) {
			zigbee_ep_set_temperature(ZIGBEE_TEMP_EXTRACT,
				(int16_t)(st.temp_extract_air_x10 * 10));
		}

		if (st.mode <= 3) {
			zigbee_ep_set_mode((uint8_t)st.mode);
		}
	}

	k_work_reschedule(&bridge_work, K_SECONDS(BRIDGE_POLL_INTERVAL_S));
}

int flexit_bridge_init(void)
{
	zigbee_ep_set_mode_write_handler(mode_write_handler);
	k_work_schedule(&bridge_work, K_SECONDS(BRIDGE_POLL_INTERVAL_S));
	LOG_INF("Flexit<->Zigbee bridge started (poll %ds)", BRIDGE_POLL_INTERVAL_S);
	return 0;
}
