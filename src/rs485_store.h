#ifndef FLEXITMC3_RS485_STORE_H_
#define FLEXITMC3_RS485_STORE_H_

#include <stddef.h>
#include <stdint.h>

/* Capacity of the in-RAM RS485 ring (includes 2-byte per-frame length prefix
 * overhead, so usable payload is a bit less). 2 KiB lines up with the spec
 * the BLE console dump path is built against.
 */
#define RS485_STORE_SIZE 2048

/* Append one RS485 frame to the ring. Frames are stored length-prefixed so a
 * reader can reconstruct them byte-for-byte. When the ring is too full to fit
 * the new frame, the oldest frames are dropped until there's room. Frames
 * larger than the ring (impossible in practice) are silently discarded.
 */
void rs485_store_append(const uint8_t *data, size_t len);

/* Copy the current ring contents into `out` (up to `max_out` bytes). The
 * returned bytes are length-prefixed frames in append order; pass them
 * through rs485_store_iter_next() or parse the [len_lo][len_hi][data] layout
 * directly. Returns the number of bytes copied.
 */
size_t rs485_store_snapshot(uint8_t *out, size_t max_out);

#endif
