#ifndef FLEXITMC3_RS485_UART_H_
#define FLEXITMC3_RS485_UART_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize RS485 UART receive on the alias 'rs485-uart' (uart0).
 * Sets the DE/RE GPIO to receive mode (LOW = high-Z), enables the async
 * UART API with double-buffered DMA and feeds incoming bytes into the
 * rs485_store and panel_mirror_feed/flexit_slave_feed.
 * Returns 0 on success, negative errno on failure.
 */
int rs485_uart_init(void);

/* Synchronously transmit a frame on the RS485 bus.
 *
 * Raises DE to drive the bus, transmits via the UART async TX API, waits
 * for completion (UART_TX_DONE/UART_TX_ABORTED), and lowers DE again. The
 * call serializes with itself via an internal mutex; concurrent transmits
 * are queued. Returns 0 on success, negative errno on failure.
 *
 * Caller-supplied buffer must remain valid until this function returns.
 */
int rs485_uart_send(const uint8_t *frame, size_t len);

#endif /* FLEXITMC3_RS485_UART_H_ */
