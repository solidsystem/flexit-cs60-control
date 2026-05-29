#ifndef ZIGBEE_EP_H
#define ZIGBEE_EP_H

#include <stdint.h>

/* Register the Zigbee endpoint(s) and start the ZBOSS stack (end device).
 * Must be called once at boot, after the BLE stack is up (they share the
 * radio via MPSL). Returns 0 on success.
 */
int zigbee_ep_init(void);

/* Publish a temperature reading (hundredths of a degree Celsius, ZCL
 * MeasuredValue encoding) to the Temperature Measurement cluster.
 * Phase 2 will feed this from the decoded CS60 state. No-op until the
 * stack is started.
 */
void zigbee_ep_set_temperature(int16_t centi_celsius);

#endif /* ZIGBEE_EP_H */
