#ifndef FLEXITMC3_BLE_TRANSPORT_H_
#define FLEXITMC3_BLE_TRANSPORT_H_

/* Bring up BLE peripheral with:
 *   - SMP-over-BT service for DFU (authenticated/paired access only).
 *   - Nordic UART Service for console output mirroring.
 *   - Fixed passkey for pairing; security level L3 forced on connect.
 * Returns 0 on success, negative errno on failure.
 */
int ble_transport_init(void);

#endif
