/* Bridge the RS485 panel mirror to the Zigbee data model.
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

/* Setpoint write (ZHA -> CS60): value_dc is °C ×10, matching both the CS60
 * register encoding and flexit_slave_queue_setpoint().
 */
static void setpoint_write_handler(int16_t value_dc)
{
	int err = flexit_slave_queue_setpoint((uint16_t)value_dc);
	if (err) {
		LOG_WRN("flexit_slave_queue_setpoint(%d) failed: %d", value_dc, err);
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

		/* Intake (outdoor) air temp on an Analog Input EP (°C ×10). */
		zigbee_ep_set_intake_temp(st.temp_outdoor_air_x10);

		/* HX modulation + heater output, raw percentage 0..100. */
		zigbee_ep_set_percent(ZIGBEE_ANALOG_HEAT_EXCHANGER,
			st.pct_heat_exchanger);
		zigbee_ep_set_percent(ZIGBEE_ANALOG_HEATING,
			st.pct_heating);

		/* Setpoint readback: the committed value (0x00C2), already °C ×10. */
		zigbee_ep_set_setpoint(st.temp_setpoint_2_x10);

		/* Alarm flags (FC10 regs 0x0104..0x010C) -> Binary Input sensors. */
		zigbee_ep_set_binary(ZIGBEE_BINARY_SUPPLY_SENSOR,
			(st.alarms & PANEL_MIRROR_ALARM_SUPPLY_SENSOR) != 0);
		zigbee_ep_set_binary(ZIGBEE_BINARY_OUTDOOR_SENSOR,
			(st.alarms & PANEL_MIRROR_ALARM_OUTDOOR_SENSOR) != 0);
		zigbee_ep_set_binary(ZIGBEE_BINARY_HEAT_EXCHANGER,
			(st.alarms & PANEL_MIRROR_ALARM_HEAT_EXCHANGER) != 0);
		zigbee_ep_set_binary(ZIGBEE_BINARY_OVERHEAT,
			(st.alarms & PANEL_MIRROR_ALARM_OVERHEAT) != 0);
		zigbee_ep_set_binary(ZIGBEE_BINARY_FILTER,
			(st.alarms & PANEL_MIRROR_ALARM_FILTER) != 0);

		if (st.mode <= 3) {
			zigbee_ep_set_mode((uint8_t)st.mode);
		}
	}

	k_work_reschedule(&bridge_work, K_SECONDS(BRIDGE_POLL_INTERVAL_S));
}

int flexit_bridge_init(void)
{
	zigbee_ep_set_mode_write_handler(mode_write_handler);
	zigbee_ep_set_setpoint_write_handler(setpoint_write_handler);
	k_work_schedule(&bridge_work, K_SECONDS(BRIDGE_POLL_INTERVAL_S));
	LOG_INF("Flexit<->Zigbee bridge started (poll %ds)", BRIDGE_POLL_INTERVAL_S);
	return 0;
}
