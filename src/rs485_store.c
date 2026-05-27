#include "rs485_store.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

/*
 * Plain circular byte buffer — no length-prefix framing.
 *
 * Invariants:
 *   head  = index of the NEXT write position (0 … RS485_STORE_SIZE-1)
 *   fill  = number of valid bytes stored     (0 … RS485_STORE_SIZE)
 *
 * Oldest byte is always at:
 *   (head + RS485_STORE_SIZE - fill) % RS485_STORE_SIZE
 *
 * When fill == RS485_STORE_SIZE the buffer is full; new bytes overwrite
 * the oldest content automatically — no explicit eviction step, so there
 * is no framing state that can be corrupted.
 */
static uint8_t  ring[RS485_STORE_SIZE];
static uint32_t head;
static uint32_t fill;
static K_MUTEX_DEFINE(store_mutex);

void rs485_store_append(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    /* Clip: if the burst is larger than the ring, keep only the tail. */
    if (len > RS485_STORE_SIZE) {
        data += len - RS485_STORE_SIZE;
        len   = RS485_STORE_SIZE;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);

    uint32_t n     = (uint32_t)len;
    uint32_t first = MIN(RS485_STORE_SIZE - head, n);

    memcpy(ring + head, data, first);
    if (first < n) {
        memcpy(ring, data + first, n - first);
    }
    head = (head + n) % RS485_STORE_SIZE;
    fill = MIN(fill + n, (uint32_t)RS485_STORE_SIZE);

    k_mutex_unlock(&store_mutex);
}

size_t rs485_store_snapshot(uint8_t *out, size_t max_out)
{
    k_mutex_lock(&store_mutex, K_FOREVER);

    uint32_t n     = MIN((uint32_t)fill, (uint32_t)max_out);
    uint32_t start = (head + RS485_STORE_SIZE - fill) % RS485_STORE_SIZE;
    uint32_t first = MIN(RS485_STORE_SIZE - start, n);

    memcpy(out, ring + start, first);
    if (first < n) {
        memcpy(out + first, ring, n - first);
    }

    k_mutex_unlock(&store_mutex);
    return (size_t)n;
}
