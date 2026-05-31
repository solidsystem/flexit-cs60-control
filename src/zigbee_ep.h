#ifndef ZIGBEE_EP_H
#define ZIGBEE_EP_H

#include <stdbool.h>
#include <stdint.h>

/* Zigbee data model for the flexit-cs60-control bridge (see smarthouse-integration.md).
 *
 * Endpoints:
 *   EP1  Basic + Identify + Fan Control  — current mode (read) + set mode (write)
 *   EP2  Temperature Measurement         — supply air
 *   EP4  Analog Input (Basic)            — intake air temperature (°C)
 *   EP5  Analog Input (Basic)            — heat-exchanger modulation (%)
 *   EP6  Analog Input (Basic)            — heating output (%)
 *   EP7  Analog Value (Basic)            — temperature setpoint (read/write)
 *
 * EP4 is an Analog Input rather than a Temperature Measurement cluster so HA
 * can give it a distinct entity name from its cluster Description ("Intake air
 * temperature"); the Temperature Measurement cluster has no name attribute, so
 * EP2/EP4 would otherwise both surface as a generic "Temperature".
 *
 * Cluster values are fed through the setters below from any thread; they are
 * latched and pushed into the ZCL attributes from the Zigbee stack thread
 * (attribute writes must happen in ZBOSS context). The setters are driven from
 * the RS485 decode (src/flexit_bridge.c).
 */

/* Temperature Measurement channels — see temp_ep_id[] in zigbee_ep.c. */
enum zigbee_temp_channel {
	ZIGBEE_TEMP_SUPPLY = 0,  /* supply air   (EP2) */
	ZIGBEE_TEMP_COUNT,
};

/* Analog Input channels — see analog_ep_id[] in zigbee_ep.c. The first two are
 * percentages (PresentValue 0..100); the last is intake air temperature in °C.
 */
enum zigbee_analog_channel {
	ZIGBEE_ANALOG_HEAT_EXCHANGER = 0, /* rotary HX modulation (EP5, %)   */
	ZIGBEE_ANALOG_HEATING,            /* heater output        (EP6, %)   */
	ZIGBEE_ANALOG_INTAKE_TEMP,        /* intake air temp      (EP4, °C)  */
	ZIGBEE_ANALOG_COUNT,
};

/* Binary Input (alarm) channels — see binary_ep_id[] in zigbee_ep.c. Each is a
 * ZCL Binary Input cluster carried *alongside* an existing endpoint's primary
 * cluster: the 8-endpoint ZBOSS cap (CONFIG_ZB_MAX_EP_NUMBER) rules out a
 * dedicated endpoint per alarm, so they ride on EP1/EP2/EP4/EP5/EP6. ZHA still
 * discovers each Binary Input as its own binary_sensor, named from the cluster
 * Description attribute.
 */
enum zigbee_binary_channel {
	ZIGBEE_BINARY_SUPPLY_SENSOR = 0,  /* supply air sensor faulty  (EP2) */
	ZIGBEE_BINARY_OUTDOOR_SENSOR,     /* outdoor air sensor faulty (EP4) */
	ZIGBEE_BINARY_HEAT_EXCHANGER,     /* heat exchanger faulty     (EP5) */
	ZIGBEE_BINARY_OVERHEAT,           /* overheat triggered        (EP6) */
	ZIGBEE_BINARY_FILTER,             /* filter change             (EP1) */
	ZIGBEE_BINARY_COUNT,
};

/* Coarse network state, polled by the main loop to drive the status LED.
 * Updated from the ZBOSS signal handler; read via zigbee_ep_net_state().
 */
enum zigbee_net_state {
	ZIGBEE_NET_IDLE = 0, /* not joined and the join window is closed       */
	ZIGBEE_NET_JOINING,  /* unjoined, join (pairing) window currently open */
	ZIGBEE_NET_JOINED,   /* joined to a coordinator's network             */
};

/* Invoked (in a Zephyr thread context, from the ZBOSS stack thread) when a
 * Zigbee client writes Fan Control FanMode. `flexit_mode` is 0..3
 * (Stop/Min/Normal/Max). flexit_bridge points this at flexit_slave_queue_mode();
 * if left unset, zigbee_ep logs the request as a stub CMD_MODE sink.
 */
typedef void (*zigbee_ep_mode_write_cb_t)(uint8_t flexit_mode);

/* Invoked (from the ZBOSS stack thread) when a Zigbee client writes the
 * setpoint Analog Value PresentValue. `value_dc` is the requested temperature
 * in tenths of a degree Celsius (°C ×10), matching the CS60 register encoding.
 * flexit_bridge points this at flexit_slave_queue_setpoint().
 */
typedef void (*zigbee_ep_setpoint_write_cb_t)(int16_t value_dc);

/* Register the Zigbee endpoints and start the ZBOSS stack (end device). Call
 * once at boot, after the BLE stack is up. Returns 0 on success.
 */
int zigbee_ep_init(void);

/* Publish a temperature reading for one channel, in hundredths of a degree
 * Celsius (ZCL MeasuredValue encoding). Thread-safe; no-op for a bad channel.
 */
void zigbee_ep_set_temperature(enum zigbee_temp_channel ch, int16_t centi_celsius);

/* Publish a percentage reading (0..100) for one Analog Input channel as the
 * cluster's PresentValue (float). Use only for the percentage channels
 * (ZIGBEE_ANALOG_HEAT_EXCHANGER / ZIGBEE_ANALOG_HEATING). Thread-safe; no-op for
 * a bad channel.
 */
void zigbee_ep_set_percent(enum zigbee_analog_channel ch, uint16_t percent);

/* Publish the intake-air temperature (EP4 Analog Input) as PresentValue, in
 * tenths of a degree Celsius (°C ×10, matching the CS60 register encoding).
 * Thread-safe.
 */
void zigbee_ep_set_intake_temp(int16_t value_dc);

/* Publish a Binary Input alarm flag for one channel (true = active/alarm).
 * Thread-safe; no-op for a bad channel.
 */
void zigbee_ep_set_binary(enum zigbee_binary_channel ch, bool active);

/* Publish the current Flexit mode (0..3) as Fan Control FanMode — use to
 * reflect the mode read back from the CS60. Thread-safe; no-op if mode > 3.
 */
void zigbee_ep_set_mode(uint8_t flexit_mode);

/* Register the handler called when a Zigbee client writes FanMode. */
void zigbee_ep_set_mode_write_handler(zigbee_ep_mode_write_cb_t cb);

/* Publish the current temperature setpoint read back from the CS60 as the
 * setpoint Analog Value PresentValue. `value_dc` is in tenths of a degree
 * Celsius (°C ×10). Thread-safe.
 */
void zigbee_ep_set_setpoint(int16_t value_dc);

/* Register the handler called when a Zigbee client writes the setpoint. */
void zigbee_ep_set_setpoint_write_handler(zigbee_ep_setpoint_write_cb_t cb);

/* Perform a Zigbee "factory reset" (BDB reset via local action): leave the
 * current network, clear ZBOSS persistent data, then reboot. The reboot is
 * what makes the device joinable again — a clean NVRAM boots as
 * DEVICE_FIRST_START, which auto-starts BDB network steering, whereas a plain
 * reboot with stored network state only rejoins the old network.
 *
 * Use this to re-pair with a different coordinator (e.g. move from the test
 * coordinator to Home Assistant/ZHA). Thread-safe; may be called from any
 * thread (e.g. the BLE command handler). The call returns immediately; the
 * device reboots a second or so later once the leave has been processed.
 */
void zigbee_ep_factory_reset(void);

/* Current coarse network state, for driving the status LED. Safe to call from
 * any thread (backed by an atomic). The JOINING state spans only the bounded
 * join/pairing window of an unjoined device (clean NVRAM / after zbreset),
 * which closes after CONFIG_ZIGBEE_DEV_REJOIN_TIMEOUT_MS; a stored-network
 * reboot-rejoin reports IDLE until it actually joins, never JOINING.
 */
enum zigbee_net_state zigbee_ep_net_state(void);

#endif /* ZIGBEE_EP_H */
