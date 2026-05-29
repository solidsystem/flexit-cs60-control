# Smarthouse Integration — Flexit CS60

Spec, design decisions, and implementation TODO for integrating the Flexit CS60
ventilation system into Home Assistant (HA) via the XIAO BLE (nRF52840) bridge
sitting on the RS485 bus.

## Goal

Expose the CS60's state to Home Assistant as entities so the ventilation system can
be monitored and controlled from HA dashboards and automations. Initial scope is
deliberately small (see [Zigbee scope](#zigbee-scope--data-model)):

- **Read-only:** temperature readings and current mode.
- **Writable:** mode only (Stop / Min / Normal / Max).

```
  CS60 ──RS485/Modbus── CI60 panel
    │
    └──RS485 tap───── XIAO BLE (nRF52840) ─┬─ Zigbee (802.15.4) ── HA Zigbee coordinator (ZHA/Z2M)
                                           └─ BLE ──────────────── dev host (debug + DFU only)
```

The XIAO already taps the RS485 bus, decodes panel state, and can inject `CMD_MODE`
changes (see `flexit-cs60-communication.md` and `tools/ble-client`).

---

## Architecture decision: dual-protocol (BLE + Zigbee)

**Decided.** Run **both** radios concurrently on the nRF52840, with distinct roles:

| Transport | Role | Audience |
|-----------|------|----------|
| **Zigbee** (802.15.4) | Production smarthouse integration — always-on end device exposing temps + mode | Home Assistant |
| **BLE** | Debug/diagnostics (RS485 stream, state dump) **and firmware flashing (SMP DFU)** only | Developer, on demand |

BLE is demoted from "the integration" to "the service port". It keeps everything that
already works (`tools/ble-client` stream/state/flash) without having to be reliable at
range, because the HA data path no longer depends on it.

---

## Feasibility & constraints

### Concurrent BLE + Zigbee is supported on nRF52840 — with caveats

- The nRF52840 supports **dynamic concurrent multiprotocol**: BLE and 802.15.4 (Zigbee)
  run simultaneously, time-sliced by the **Multi-Protocol Service Layer (MPSL)** with
  the SoftDevice Controller arbitrating radio timeslots — no tear-down/re-init when
  switching. This is a first-class, documented nRF52840 capability.
- Nordic's `zigbee/light_switch` sample ships an `overlay-multiprotocol_ble.conf` that
  runs a Zigbee end device **and** a BLE GATT service (NUS) at the same time, "without
  breaking connection from any of the used radio protocols" — i.e. the exact shape we
  want, just with SMP instead of NUS on the BLE side.

### Other constraints to validate

- **SMP-over-BLE + Zigbee together.** *Compile proven in Phase 0:* the
  `overlay-multiprotocol_ble.conf` build enables `CONFIG_BT` + `CONFIG_BT_SMP` + NUS +
  bonding **alongside** the Zigbee End Device and links fine. Our firmware's BLE surface
  is exactly those two standard services — **NUS** (diagnostic channel) + **SMP** (DFU);
  there is no custom GATT service. *Runtime* coexistence (a live BLE DFU while joined to
  the Zigbee mesh) still needs on-hardware validation.
- **Bonding/pairing.** BLE stays connectable for debug/DFU; confirm Zigbee join and
  BLE bonding coexist and survive resets.

---

## Zigbee scope & data model

Minimal first cut. Device role: **Zigbee End Device, rx-on-when-idle = true**
(always-on, mains-powered — not a sleepy end device), so writes land promptly and
attribute reporting can push updates.

### Clusters / endpoints

Zigbee's Temperature Measurement cluster carries a single value, so each temperature
needs its **own endpoint**. Proposed layout:

| Endpoint | Cluster | Dir | Maps to | HA entity |
|----------|---------|-----|---------|-----------|
| EP1 | Basic (0x0000), Identify (0x0003) | server | device id / mfg / model | (device info) |
| EP1 | **Fan Control (0x0202)** — `FanMode` (rw, enum8) | server | **current mode (read) + set mode (write)** | fan / select |
| EP2 | **Temperature Measurement (0x0402)** — `MeasuredValue` (r, int16, 0.01 °C) | server | temp sensor #1 | sensor |
| EP3 | Temperature Measurement (0x0402) | server | temp sensor #2 | sensor |
| EP4… | Temperature Measurement (0x0402) | server | further temps as needed | sensor |

**Mode mapping** (Flexit ↔ Fan Control `FanMode`):

| Flexit mode | `FanMode` value |
|-------------|-----------------|
| Stop   | 0 (Off) |
| Min    | 1 (Low) |
| Normal | 2 (Medium) |
| Max    | 3 (High) |

- Reading `FanMode` reports the current mode; writing it triggers the existing
  `CMD_MODE` injection path on the RS485 side (the same flow `ble-client mode N` uses).
- Fan Control is a standard HVAC cluster that ZHA/Z2M expose natively, so this gives a
  real fan/preset entity with little or no custom converter.
- *Alternative considered:* a Multistate Value/Output cluster gives an exact 4-state
  named enum, but ZHA/Z2M support it less natively. Prefer Fan Control unless the named
  states matter more than fan semantics.

Which temperatures to expose (supply / extract / outdoor / exhaust) follows whatever
`ble-client state` already decodes — pick the meaningful subset; the rest of the rich
state (runtime counters, filter timers, alarms) is **out of scope for now**.

### Attribute reporting

Configure reporting on `MeasuredValue` and `FanMode` (min/max interval + reportable
change) so HA receives push updates instead of polling.

---

## Open questions / risks

Still unverified (needs hardware + a coordinator): actual network join, mode/temperature
round-trips, a BLE DFU performed while joined to the Zigbee mesh

## TODO

### Phase 1 — Firmware: dual-protocol skeleton ✅ DONE (build + BLE on hardware); ⏳ Zigbee runtime pending
- [x] Register `ncs-zigbee` (commit `8a6c6ca`) so PM discovers the ZBOSS partitions
      (via `ZEPHYR_EXTRA_MODULES` + static `pm_static.yml`).
- [x] Enable MPSL-based BLE + 802.15.4 multiprotocol; kept `CONFIG_MCUMGR_TRANSPORT_BT`.
- [x] MCUboot dual-slot layout fitting ZBOSS NVRAM + settings — secondary in QSPI.
- [x] Define a Zigbee end-device context (HA Temperature Sensor) that compiles & links.
- [x] **(hardware)** First SWD/J-Link flash of `merged.hex`; boots, BLE adv + pair (444999)
      + NUS verified; BLE/Zigbee coexist at runtime (see Hardware verification, 2026-05-29).
- [ ] **(runtime)** Confirm the end device actually joins ZHA/Z2M (rx-on-when-idle).
- [ ] **(runtime)** Confirm BLE NUS + SMP DFU still work while Zigbee is joined.
- [ ] **(runtime)** Confirm RS485 decode populates `state` once the CS60 bus is reconnected.
- [x] Channel selection — set `CONFIG_ZIGBEE_CHANNEL_SELECTION_MODE_MULTI=y` so the end
      device scans all channels (11-26) to find the HA coordinator. Nordic's documented
      recommendation for joining a third-party (ZHA/Z2M) coordinator; no fixed channel needed.

### Phase 2 — Firmware: data model
- [ ] Implement endpoints/clusters per [data model](#clusters--endpoints).
- [ ] Wire Temperature `MeasuredValue` from the RS485 state decode (shared core).
- [ ] Wire Fan Control `FanMode`: read current mode; on write, trigger `CMD_MODE`.
- [ ] Configure attribute reporting for temps and mode.

### Phase 3 — Home Assistant
- [ ] Pair the device to ZHA (or Z2M); verify auto-discovered entities.
- [ ] Add ZHA quirk / Z2M external converter if the mode mapping needs it.
- [ ] Validate read (temps, mode) and write (set mode) end-to-end from HA.

### Phase 4 — Hardening
- [ ] Availability / stale-data handling in HA when the bridge or mesh drops.
- [ ] Confirm DFU over BLE while the device is live on the Zigbee network.
- [ ] Document the operational runbook (join, re-join after DFU, debug via BLE).

---

## References

- `flexit-cs60-communication.md` — reverse-engineered RS485/Modbus protocol.
- `tools/ble-client` — existing BLE tooling (stream/state/mode/flash).
- HA ZHA integration: https://www.home-assistant.io/integrations/zha/
