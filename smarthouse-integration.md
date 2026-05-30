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

Network join is **verified** (2026-05-29, see Phase 1). Still unverified: mode/temperature
round-trips (Phase 2 data model not yet wired) and a BLE DFU performed while joined to the
Zigbee mesh.

## TODO

Ordered by dependency: everything that runs on the **current bench** (the `tools/zb-coordinator`
test coordinator is up; the `hci_usb` BLE adapter and the CS60/RS485 bus are **disconnected**)
comes first, so all the Zigbee work is finished before the steps that need BLE or RS485 wired
back in. Phases 2–3 need neither; Phase 4 needs the CS60 bus; Phase 5 needs the BLE adapter.

### Phase 1 — Firmware: dual-protocol skeleton ✅ DONE
- [x] Register `ncs-zigbee` (commit `8a6c6ca`) so PM discovers the ZBOSS partitions
      (via `ZEPHYR_EXTRA_MODULES` + static `pm_static.yml`).
- [x] Enable MPSL-based BLE + 802.15.4 multiprotocol; kept `CONFIG_MCUMGR_TRANSPORT_BT`.
- [x] MCUboot dual-slot layout fitting ZBOSS NVRAM + settings — secondary in QSPI.
- [x] Define a Zigbee end-device context (HA Temperature Sensor) that compiles & links.
- [x] **(hardware)** First SWD/J-Link flash of `merged.hex`; boots, BLE adv + pair (444999)
      + NUS verified; BLE/Zigbee coexist at runtime (see Hardware verification, 2026-05-29).
- [x] **(runtime)** Confirm the end device actually joins a coordinator (rx-on-when-idle).
      ✅ 2026-05-29: joined the Nordic `network_coordinator` test coordinator
      (`tools/zb-coordinator`, flashed on the nrf52840dk) — PAN 0x4716, confirmed from both
      consoles (coordinator "New device commissioned"; XIAO "Joined network successfully").
      Joining a real ZHA/Z2M coordinator is Phase 3.
- [x] Channel selection — set `CONFIG_ZIGBEE_CHANNEL_SELECTION_MODE_MULTI=y` so the end
      device scans all channels (11-26) to find the HA coordinator. Nordic's documented
      recommendation for joining a third-party (ZHA/Z2M) coordinator; no fixed channel needed.

### Phase 2 — Zigbee data model + bench verification ✅ DONE (2026-05-30; reporting caveat)
Implemented in `src/zigbee_ep.c` and exercised with **synthetic values**, decoupled from the
RS485 decode, so the radio side is finished before the CS60 bus is reconnected (Phase 4 swaps the
synthetic feeds for real data). Verified over the air against the `tools/zb-shell` coordinator.
- [x] Implement endpoints/clusters per [data model](#clusters--endpoints): EP1 Basic + Identify +
      Fan Control (`FanMode` rw, made reportable by hand), EP2/EP3/EP4 Temperature Measurement
      (supply / extract / outdoor). Hand-declared (not the canned HA device-type macro) so EP1 can
      carry Fan Control and the temp EPs share one simple-descriptor type.
- [x] Drive the clusters with synthetic values (temperature ramp + slowly cycling `FanMode`) via
      the `zigbee_ep_set_*` API. Values are latched and pushed into the attributes by a publish
      tick on the ZBOSS thread — the same path Phase 4 feeds from the RS485 decode.
- [x] Wire the `FanMode` **write** path to a stub `CMD_MODE` sink (logs the mapped Flexit mode;
      Phase 4 registers `flexit_slave_queue_mode` via `zigbee_ep_set_mode_write_handler`).
      Verified: a `FanMode=2` write from the coordinator logged `FanMode write -> Flexit mode 2`.
- [x] Bench tooling: `tools/zb-shell` (ncs-zigbee `shell` sample + static PM) on the DK. It resumes
      the persisted `0x4716` network from NVRAM (`bdb role zc` / `bdb start`), so the XIAO stays
      joined. Verified reads: `FanMode`=Off, supply 22.90 °C, extract 25.50 °C, outdoor 7.40 °C.
- [x] Attribute reporting: `FanMode` + temps declared reportable; device accepts Configure
      Reporting. Confirmed live (2026-05-30): after a manual `zdo bind` of the temp cluster to the
      coordinator, the XIAO streamed unsolicited reports (~10 in 30 s as the synthetic ramp moved).
      ZBOSS only reports to **bound** destinations, so the binding is required — ZHA/Z2M create it
      automatically; the bench `zcl subscribe` alone does not.

### Phase 3 — Home Assistant pairing (needs a ZHA/Z2M coordinator; no BLE / no CS60 needed)
Prep done off-HA (2026-05-30); the remaining items need a live ZHA/Z2M coordinator in HA.
- [x] Device identity: added ManufacturerName `SolidSystem` + ModelIdentifier `flexitMC` to the
      Basic cluster (read back over the air) so HA names the device and Z2M can match a converter.
- [x] Authored `tools/ha/` artifacts: a ZHA v2 quirk (`zha_quirk_flexitmc.py`, friendly
      supply/extract/outdoor names), a Z2M external converter (`zigbee2mqtt_flexitmc.js`), and a
      `README.md` runbook with the exact Zigbee signature. **Untested** against a live HA/Z2M.
- [x] Live attribute reporting — proven at the protocol level via the bench shell (see Phase 2);
      ZHA/Z2M will set up the same binding automatically.
- [ ] Pair the device to ZHA (or Z2M) on real HA hardware; verify the auto-discovered entities
      (one device: a fan + three temperature sensors) and that reads/writes/reports work in the UI.
- [ ] Validate / tweak the quirk + converter against your ZHA (zigpy) and Z2M versions; add the
      friendly temp names (or just rename in the HA UI).

### Phase 4 — RS485 integration (needs the CS60 bus reconnected)
- [ ] Wire Temperature `MeasuredValue` from the real RS485 state decode (shared core) — replaces
      the Phase 2 synthetic feed.
- [ ] Wire Fan Control `FanMode`: read current mode; on write, trigger the real `CMD_MODE`
      injection — replaces the Phase 2 stub sink.
- [ ] Confirm RS485 decode populates `state` once the CS60 bus is reconnected.
- [ ] End-to-end: set mode over Zigbee → CS60 reacts; CS60 temps change → seen over Zigbee and in HA.

### Phase 5 — BLE service-port + hardening (needs the `hci_usb` adapter restored)
Reflash `hci_usb` to the DK (`west flash --build-dir build-hci-usb`); this tears down the test
coordinator, so run these only after the Zigbee/RS485 work above is complete.
- [ ] **(runtime)** Confirm BLE NUS + SMP DFU still work while Zigbee is joined.
- [ ] Confirm DFU over BLE while the device is live on the Zigbee network.
- [ ] Availability / stale-data handling in HA when the bridge or mesh drops.
- [ ] Document the operational runbook (join, re-join after DFU, debug via BLE).

---

## References

- `flexit-cs60-communication.md` — reverse-engineered RS485/Modbus protocol.
- `tools/ble-client` — existing BLE tooling (stream/state/mode/flash).
- `tools/zb-coordinator` — test Zigbee coordinator (ncs-zigbee `network_coordinator` + a static
  PM file) for the nrf52840dk; used to verify the end-device join. Build with
  `-DZEPHYR_EXTRA_MODULES=$HOME/ncs/v3.3.0/ncs-zigbee` (after `--`), flash with `west flash`.
  Note: this replaces the `hci_usb` BLE adapter on the DK, so `ble-client` is offline while the
  coordinator runs (restore with `west flash --build-dir build-hci-usb`).
- `tools/zb-shell` — ZCL-capable bench coordinator (ncs-zigbee `shell` sample + static PM) for the
  nrf52840dk. Resumes the persisted network from NVRAM (`bdb role zc` / `bdb start`); drive ZCL over
  `/dev/ttyACM0` (`zcl attr read/write`, `zdo bind`, `zcl subscribe`). Used to verify Phase 2/3.
- `tools/ha` — Home Assistant integration artifacts: ZHA v2 quirk, Z2M external converter, and a
  runbook with the device's Zigbee signature.
- HA ZHA integration: https://www.home-assistant.io/integrations/zha/
