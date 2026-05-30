#ifndef ZIGBEE_EP_H
#define ZIGBEE_EP_H

#include <stdint.h>

/* Zigbee data model for the flexitMC bridge (smarthouse-integration.md Phase 2).
 *
 * Endpoints:
 *   EP1  Basic + Identify + Fan Control  — current mode (read) + set mode (write)
 *   EP2  Temperature Measurement         — supply air
 *   EP4  Temperature Measurement         — outdoor air
 *
 * Cluster values are fed through the setters below from any thread; they are
 * latched and pushed into the ZCL attributes from the Zigbee stack thread
 * (attribute writes must happen in ZBOSS context). Phase 2 drives them with a
 * synthetic generator; Phase 4 feeds the same setters from the RS485 decode.
 */

/* Temperature channels — see temp_ep_id[] in zigbee_ep.c for the EP mapping. */
enum zigbee_temp_channel {
	ZIGBEE_TEMP_SUPPLY = 0,  /* supply air   (EP2) */
	ZIGBEE_TEMP_OUTDOOR,     /* outdoor air  (EP4) */
	ZIGBEE_TEMP_COUNT,
};

/* Invoked (in a Zephyr thread context, from the ZBOSS stack thread) when a
 * Zigbee client writes Fan Control FanMode. `flexit_mode` is 0..3
 * (Stop/Min/Normal/Max). Phase 4 points this at flexit_slave_queue_mode(); if
 * left unset, zigbee_ep logs the request as a stub CMD_MODE sink (Phase 2).
 */
typedef void (*zigbee_ep_mode_write_cb_t)(uint8_t flexit_mode);

/* Register the Zigbee endpoints and start the ZBOSS stack (end device). Call
 * once at boot, after the BLE stack is up. Returns 0 on success.
 */
int zigbee_ep_init(void);

/* Publish a temperature reading for one channel, in hundredths of a degree
 * Celsius (ZCL MeasuredValue encoding). Thread-safe; no-op for a bad channel.
 */
void zigbee_ep_set_temperature(enum zigbee_temp_channel ch, int16_t centi_celsius);

/* Publish the current Flexit mode (0..3) as Fan Control FanMode — use to
 * reflect the mode read back from the CS60. Thread-safe; no-op if mode > 3.
 */
void zigbee_ep_set_mode(uint8_t flexit_mode);

/* Register the handler called when a Zigbee client writes FanMode. */
void zigbee_ep_set_mode_write_handler(zigbee_ep_mode_write_cb_t cb);

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

#endif /* ZIGBEE_EP_H */
