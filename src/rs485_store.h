#ifndef FLEXITMC3_RS485_STORE_H_
#define FLEXITMC3_RS485_STORE_H_

#include <stddef.h>
#include <stdint.h>

/* Capacity of the in-RAM RS485 ring buffer (raw bytes, no framing overhead). */
#define RS485_STORE_SIZE 2048

/* Append raw bytes to the ring. When full, the oldest bytes are silently
 * overwritten. Inputs larger than RS485_STORE_SIZE are clipped to keep only
 * the most-recent RS485_STORE_SIZE bytes. Thread-safe.
 */
void rs485_store_append(const uint8_t *data, size_t len);

/* Copy the current ring contents (oldest byte first) into `out`, up to
 * `max_out` bytes. Returns the number of bytes copied. Thread-safe.
 */
size_t rs485_store_snapshot(uint8_t *out, size_t max_out);

#endif /* FLEXITMC3_RS485_STORE_H_ */
