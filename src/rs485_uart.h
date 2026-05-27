#ifndef FLEXITMC3_RS485_UART_H_
#define FLEXITMC3_RS485_UART_H_

/* Initialize RS485 UART receive on the alias 'rs485-uart' (uart0).
 * Sets the DE/RE GPIO to receive mode (LOW = high-Z), installs an
 * interrupt-driven receive handler that feeds bytes into the rs485_store
 * via an inter-frame silence timeout (5 ms): when no byte arrives for
 * 5 ms the accumulated bytes are committed as one frame.
 * Returns 0 on success, negative errno on failure.
 */
int rs485_uart_init(void);

#endif /* FLEXITMC3_RS485_UART_H_ */
