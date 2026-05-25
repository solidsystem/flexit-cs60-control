#include "rs485_store.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

static uint8_t store[RS485_STORE_SIZE];
static uint32_t head;        /* next write offset */
static uint32_t valid_start; /* offset of oldest stored byte */
static uint32_t valid_len;   /* number of valid bytes currently in `store` */
static K_MUTEX_DEFINE(store_mutex);

static void buf_write(uint32_t pos, const uint8_t *src, uint32_t n)
{
    uint32_t first = MIN(RS485_STORE_SIZE - pos, n);
    memcpy(store + pos, src, first);
    if (first < n) {
        memcpy(store, src + first, n - first);
    }
}

static void buf_read(uint32_t pos, uint8_t *dst, uint32_t n)
{
    uint32_t first = MIN(RS485_STORE_SIZE - pos, n);
    memcpy(dst, store + pos, first);
    if (first < n) {
        memcpy(dst + first, store, n - first);
    }
}

static void drop_oldest_frame(void)
{
    uint8_t hdr[2];
    buf_read(valid_start, hdr, 2);
    uint16_t flen = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    uint32_t total = 2 + flen;
    valid_start = (valid_start + total) % RS485_STORE_SIZE;
    valid_len -= total;
}

void rs485_store_append(const uint8_t *data, size_t len)
{
    if (len == 0 || len > 0xFFFFu) {
        return;
    }
    uint32_t total = 2u + (uint32_t)len;
    if (total > RS485_STORE_SIZE) {
        return;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);

    while (valid_len + total > RS485_STORE_SIZE) {
        drop_oldest_frame();
    }

    uint8_t hdr[2] = {
        (uint8_t)(len & 0xFFu),
        (uint8_t)((len >> 8) & 0xFFu),
    };
    buf_write(head, hdr, 2);
    head = (head + 2) % RS485_STORE_SIZE;
    buf_write(head, data, (uint32_t)len);
    head = (head + (uint32_t)len) % RS485_STORE_SIZE;
    valid_len += total;

    k_mutex_unlock(&store_mutex);
}

size_t rs485_store_snapshot(uint8_t *out, size_t max_out)
{
    k_mutex_lock(&store_mutex, K_FOREVER);
    size_t n = MIN((size_t)valid_len, max_out);
    buf_read(valid_start, out, (uint32_t)n);
    k_mutex_unlock(&store_mutex);
    return n;
}
