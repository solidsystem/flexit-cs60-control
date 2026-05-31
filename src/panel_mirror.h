#ifndef FLEXITMC_PANEL_MIRROR_H_
#define FLEXITMC_PANEL_MIRROR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_MIRROR_COUNTER_SLOTS 8

/* Mirrored panel state derived from passive observation of the CS60's
 * Modbus broadcasts. Temperatures are stored as the raw int16 from the
 * wire (× 10 °C) so this module pulls in no float math; the host side
 * divides for display.
 */
struct panel_mirror_state {
    /* FC10 broadcast @ 0x00BE — main status block. */
    int16_t  temp_setpoint_x10;     /* 0x00BE */
    uint16_t mode;                  /* 0x00BF: 0=Stop 1=Min 2=Normal 3=Max */
    uint16_t unknown_1;             /* 0x00C0 — raw, surfaced as-is */
    uint16_t unknown_2;             /* 0x00C1 */
    int16_t  temp_setpoint_2_x10;   /* 0x00C2 */
    int16_t  temp_supply_air_x10;   /* 0x00C3 */
    int16_t  temp_extract_air_x10;  /* 0x00C4 — may be "not present" sentinel */
    int16_t  temp_outdoor_air_x10;  /* 0x00C5 */
    int16_t  temp_return_water_x10; /* 0x00C6 — may be "not present" sentinel */
    uint16_t pct_cooling;           /* 0x00C7 */
    uint16_t pct_heat_exchanger;    /* 0x00C8 */
    uint16_t pct_heating;           /* 0x00C9 */
    uint16_t pct_supply_fan;        /* 0x00CA */

    /* Sentinel presence flags: bit set = sensor present, clear = n/a. */
    uint8_t  sensors_present;
#define PANEL_MIRROR_SENSOR_EXTRACT_AIR  0x01
#define PANEL_MIRROR_SENSOR_RETURN_WATER 0x02

    /* Alarm/status registers carried later in the same FC10 block (each holding
     * register is 0 = clear, non-zero = active). Only the subset surfaced over
     * Zigbee is decoded; addresses from the ESPHome reference register map
     * (REG_ALARM_* at 0x0104..0x010C). See flexit-cs60-communication.md. */
    uint8_t  alarms;
#define PANEL_MIRROR_ALARM_SUPPLY_SENSOR  0x01  /* 0x0104 supply air sensor faulty  */
#define PANEL_MIRROR_ALARM_OUTDOOR_SENSOR 0x02  /* 0x0106 outdoor air sensor faulty */
#define PANEL_MIRROR_ALARM_OVERHEAT       0x04  /* 0x0108 overheat triggered        */
#define PANEL_MIRROR_ALARM_HEAT_EXCHANGER 0x08  /* 0x010B heat exchanger faulty     */
#define PANEL_MIRROR_ALARM_FILTER         0x10  /* 0x010C filter change             */

    /* FC06 — sparse table of last-seen single-register writes. Populated
     * lazily; reused LRU on overflow.
     */
    struct {
        uint16_t addr;
        uint16_t value;
        uint64_t at_ms;
        bool     used;
    } counters[PANEL_MIRROR_COUNTER_SLOTS];

    /* Diagnostics. */
    uint32_t fc10_frames;
    uint32_t fc06_frames;
    uint32_t crc_failures;
    uint64_t last_fc10_uptime_ms;
};

/* Feed raw RS485 bytes from the drain worker. Internally maintains a
 * small accumulator and decodes FC10/FC06 broadcasts as their boundaries
 * align. Thread-safe.
 */
void panel_mirror_feed(const uint8_t *data, size_t len);

/* Atomic copy of the current mirror into *out. Thread-safe. */
void panel_mirror_snapshot(struct panel_mirror_state *out);

#endif /* FLEXITMC_PANEL_MIRROR_H_ */
