#ifndef FLEXITMC3_BLE_TRANSPORT_H_
#define FLEXITMC3_BLE_TRANSPORT_H_

/* Bring up BLE peripheral with:
 *   - SMP-over-BT service for DFU (authenticated/paired access only).
 *   - Nordic UART Service for console output mirroring.
 *   - Fixed passkey for pairing; security level L3 forced on connect.
 * Returns 0 on success, negative errno on failure.
 */
int ble_transport_init(void);

/* Idempotent safety-net call: starts advertising if no BLE connection is
 * currently up. Returning -EALREADY is treated as success and silent. Call
 * this periodically from the main loop so that any path where the post-
 * disconnect advertising restart is missed gets corrected quickly.
 */
void ble_transport_ensure_advertising(void);

#endif
