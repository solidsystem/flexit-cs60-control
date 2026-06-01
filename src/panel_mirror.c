#include "panel_mirror.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* Modbus broadcast frame shapes we decode.
 *
 *   FC10 status block:
 *     00 10 00 BE 00 55 AA <170 data bytes> CRClo CRChi   (179 B total)
 *     addr=0, fc=0x10, start=0x00BE, qty=85, byte_count=170.
 *
 *   FC06 single-reg counter write:
 *     00 06 AH AL VH VL CRClo CRChi                       (  8 B total)
 *     addr=0, fc=0x06, register address & value follow.
 *
 * The CS60 deliberately ignores Modbus RTU inter-frame timing, so the
 * receive path cannot rely on silence to find boundaries. We instead
 * accumulate into a small ring and validate candidate frames by their
 * known opening signature plus the CRC at the expected offset; on CRC
 * failure we slip one byte and re-scan.
 */
#define ACCUM_SIZE 256
#define FC10_LEN   179
#define FC06_LEN   8

/* Sensor-not-present sentinels observed on this installation
 * (traffic-analysis.md §4): extract-air reports either -125.0 °C or
 * -187.5 °C, return-water reports either -187.5 °C or -250.0 °C.
 * No real HVAC reading can plausibly go below -100 °C, so any raw
 * int16 ≤ -1000 (× 10 °C) is treated as "not present".
 */
#define TEMP_SENTINEL_MAX_X10 (-1000)

/* Byte offset of a holding register inside the FC10 status block data region
 * (reg 0x00BE is the first register, at offset 0). Used to reach the alarm
 * registers (0x0104..0x010C) that sit later in the same 85-register block. */
#define FC10_REG_OFF(reg) (((reg) - 0x00BE) * 2)

static uint8_t accum[ACCUM_SIZE];
static size_t  accum_len;

static struct panel_mirror_state state;
static K_MUTEX_DEFINE(state_mutex);

static inline uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return ((uint16_t)p[1] << 8) | (uint16_t)p[0];
}

static bool try_decode_fc10(const uint8_t *frame)
{
    uint16_t crc_calc = crc16_ansi(frame, FC10_LEN - 2);
    uint16_t crc_recv = get_le16(frame + FC10_LEN - 2);
    if (crc_calc != crc_recv) {
        return false;
    }

    const uint8_t *regs = frame + 7;

    k_mutex_lock(&state_mutex, K_FOREVER);

    state.temp_setpoint_x10     = (int16_t)get_be16(regs + 0);
    state.mode                  = get_be16(regs + 2);
    state.unknown_1             = get_be16(regs + 4);
    state.unknown_2             = get_be16(regs + 6);
    state.temp_setpoint_2_x10   = (int16_t)get_be16(regs + 8);
    state.temp_supply_air_x10   = (int16_t)get_be16(regs + 10);
    state.temp_extract_air_x10  = (int16_t)get_be16(regs + 12);
    state.temp_outdoor_air_x10  = (int16_t)get_be16(regs + 14);
    state.temp_return_water_x10 = (int16_t)get_be16(regs + 16);
    state.pct_cooling           = get_be16(regs + 18);
    state.pct_heat_exchanger    = get_be16(regs + 20);
    state.pct_heating           = get_be16(regs + 22);
    state.pct_supply_fan        = get_be16(regs + 24);

    uint8_t present = 0;
    if (state.temp_extract_air_x10  > TEMP_SENTINEL_MAX_X10) {
        present |= PANEL_MIRROR_SENSOR_EXTRACT_AIR;
    }
    if (state.temp_return_water_x10 > TEMP_SENTINEL_MAX_X10) {
        present |= PANEL_MIRROR_SENSOR_RETURN_WATER;
    }
    state.sensors_present = present;

    /* Alarm registers later in the same block (0x0104..0x010C). Each is a
     * 0/non-zero flag; collapse the subset we surface into a bitmask. */
    uint8_t alarms = 0;
    if (get_be16(regs + FC10_REG_OFF(0x0104))) {
        alarms |= PANEL_MIRROR_ALARM_SUPPLY_SENSOR;
    }
    if (get_be16(regs + FC10_REG_OFF(0x0106))) {
        alarms |= PANEL_MIRROR_ALARM_OUTDOOR_SENSOR;
    }
    if (get_be16(regs + FC10_REG_OFF(0x0108))) {
        alarms |= PANEL_MIRROR_ALARM_OVERHEAT;
    }
    if (get_be16(regs + FC10_REG_OFF(0x010B))) {
        alarms |= PANEL_MIRROR_ALARM_HEAT_EXCHANGER;
    }
    if (get_be16(regs + FC10_REG_OFF(0x010C))) {
        alarms |= PANEL_MIRROR_ALARM_FILTER;
    }
    state.alarms = alarms;

    state.fc10_frames++;
    state.last_fc10_uptime_ms = (uint64_t)k_uptime_get();

    k_mutex_unlock(&state_mutex);
    return true;
}

static bool try_decode_fc06(const uint8_t *frame)
{
    uint16_t crc_calc = crc16_ansi(frame, FC06_LEN - 2);
    uint16_t crc_recv = get_le16(frame + FC06_LEN - 2);
    if (crc_calc != crc_recv) {
        return false;
    }

    uint16_t addr  = get_be16(frame + 2);
    uint16_t value = get_be16(frame + 4);
    uint64_t now   = (uint64_t)k_uptime_get();

    k_mutex_lock(&state_mutex, K_FOREVER);

    /* Find existing slot, else first free, else LRU. */
    int slot      = -1;
    int free_slot = -1;
    int lru_slot  = 0;
    uint64_t lru_at = state.counters[0].at_ms;

    for (int i = 0; i < PANEL_MIRROR_COUNTER_SLOTS; i++) {
        if (state.counters[i].used && state.counters[i].addr == addr) {
            slot = i;
            break;
        }
        if (!state.counters[i].used && free_slot < 0) {
            free_slot = i;
        }
        if (state.counters[i].used && state.counters[i].at_ms < lru_at) {
            lru_at   = state.counters[i].at_ms;
            lru_slot = i;
        }
    }
    if (slot < 0) {
        slot = (free_slot >= 0) ? free_slot : lru_slot;
    }

    state.counters[slot].addr  = addr;
    state.counters[slot].value = value;
    state.counters[slot].at_ms = now;
    state.counters[slot].used  = true;

    state.fc06_frames++;

    k_mutex_unlock(&state_mutex);
    return true;
}

static void bump_crc_failure(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state.crc_failures++;
    k_mutex_unlock(&state_mutex);
}

/* Try to decode one frame starting at accum[0].
 * Returns: number of bytes consumed, OR 0 if not yet decidable (need more bytes).
 */
static size_t try_decode_at_front(void)
{
    if (accum_len < 2) {
        return 0;
    }

    /* FC10 broadcast — anchor on the full 7-byte header before committing. */
    if (accum[0] == 0x00 && accum[1] == 0x10) {
        if (accum_len < 7) {
            return 0;
        }
        if (accum[2] == 0x00 && accum[3] == 0xBE &&
            accum[4] == 0x00 && accum[5] == 0x55 &&
            accum[6] == 0xAA) {
            if (accum_len < FC10_LEN) {
                return 0;
            }
            if (try_decode_fc10(accum)) {
                return FC10_LEN;
            }
            bump_crc_failure();
            return 1;
        }
        /* `00 10` but the rest doesn't match a real FC10 header — slip. */
        return 1;
    }

    /* FC06 broadcast — short frame, no further header anchor available. */
    if (accum[0] == 0x00 && accum[1] == 0x06) {
        if (accum_len < FC06_LEN) {
            return 0;
        }
        if (try_decode_fc06(accum)) {
            return FC06_LEN;
        }
        bump_crc_failure();
        return 1;
    }

    /* Unknown opening pair — slip. */
    return 1;
}

void panel_mirror_feed(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    if (len >= ACCUM_SIZE) {
        /* Burst bigger than the accumulator — keep only the tail. */
        memcpy(accum, data + (len - ACCUM_SIZE), ACCUM_SIZE);
        accum_len = ACCUM_SIZE;
    } else {
        if (accum_len + len > ACCUM_SIZE) {
            size_t drop = (accum_len + len) - ACCUM_SIZE;
            memmove(accum, accum + drop, accum_len - drop);
            accum_len -= drop;
        }
        memcpy(accum + accum_len, data, len);
        accum_len += len;
    }

    while (accum_len > 0) {
        size_t consumed = try_decode_at_front();
        if (consumed == 0) {
            break; /* need more bytes */
        }
        if (consumed < accum_len) {
            memmove(accum, accum + consumed, accum_len - consumed);
        }
        accum_len -= consumed;
    }
}

void panel_mirror_snapshot(struct panel_mirror_state *out)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    memcpy(out, &state, sizeof(*out));
    k_mutex_unlock(&state_mutex);
}

bool panel_mirror_cs60_link_up(uint32_t stale_ms)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    uint64_t last = state.last_fc10_uptime_ms;
    k_mutex_unlock(&state_mutex);

    if (last == 0) {
        return false; /* no FC10 broadcast decoded since boot */
    }
    return ((uint64_t)k_uptime_get() - last) <= (uint64_t)stale_ms;
}
