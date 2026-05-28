#ifndef FLEXITMC3_BLE_TRANSPORT_H_
#define FLEXITMC3_BLE_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

/* Bring up BLE peripheral with:
 *   - SMP-over-BT service for DFU (authenticated/paired access only).
 *   - Nordic UART Service for commands and RS485 data streaming.
 *   - Fixed passkey for pairing; security level L3 forced on connect.
 * Returns 0 on success, negative errno on failure.
 */
int ble_transport_init(void);

/* Idempotent: starts advertising if no BLE connection is currently up. */
void ble_transport_ensure_advertising(void);

/* Forward raw RS485 bytes over NUS TX if a client has requested streaming
 * (sent the "stream" command). Called from the rs485_uart drain work handler
 * on the system workqueue. Bytes are dropped silently if the BLE TX pool is
 * exhausted.
 */
void ble_transport_forward_rs485(const uint8_t *data, size_t len);

#endif /* FLEXITMC3_BLE_TRANSPORT_H_ */
