# Smarthouse Integration — Flexit CS60

Spec, design decisions, and implementation TODO for integrating the Flexit CS60
ventilation system into Home Assistant (HA) via the XIAO BLE (nRF52840) bridge
sitting on the RS485 bus.

## Goal

Expose the CS60's state to Home Assistant as entities so the ventilation system can
be monitored and controlled from HA dashboards and automations. Initial scope is
deliberately small (see [Zigbee scope](#zigbee-scope--data-model)):

- **Read-only:** temperature readings, current mode, and heat-exchanger / heating modulation (%).
- **Writable:** mode (Stop / Min / Normal / Max) and temperature setpoint (°C).

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

| Transport | Role | Audience |
|-----------|------|----------|
| **Zigbee** (802.15.4) | Production smarthouse integration — always-on end device exposing temps + mode | Home Assistant |
| **BLE** | Debug/diagnostics (RS485 stream, state dump) **and firmware flashing (SMP DFU)** only | Developer, on demand |

---


## Zigbee clusters / endpoints

Zigbee's Temperature Measurement and Analog Input clusters each carry a single
value, so every temperature and percentage needs its own endpoint. Layout:

| Endpoint | Cluster | Dir | Maps to | HA entity |
|----------|---------|-----|---------|-----------|
| EP1 | Basic (0x0000), Identify (0x0003) | server | device id / mfg / model | (device info) |
| EP1 | **Fan Control (0x0202)** — `FanMode` (rw, enum8) | server | **current mode (read) + set mode (write)** | fan / select |
| EP2 | **Temperature Measurement (0x0402)** — `MeasuredValue` (r, int16, 0.01 °C) | server | supply air temp | sensor |
| EP4 | **Analog Input (0x000C)** — `PresentValue` (r, single/float, °C) | server | intake (outdoor) air temp | sensor (native in ZHA ≥ 1.1) |
| EP5 | **Analog Input (0x000C)** — `PresentValue` (r, single/float, %) | server | heat-exchanger modulation | sensor (native in ZHA ≥ 1.1) |
| EP6 | Analog Input (0x000C) — `PresentValue` (r, single/float, %) | server | heating output | sensor (native in ZHA ≥ 1.1) |
| EP7 | **Analog Value (0x000E)** — `PresentValue` (rw, single/float, °C) | server | temperature setpoint (read + **write**) | number (ZHA needs quirk) |

EP3 (originally extract-air temperature) was removed; endpoint IDs are not renumbered.

EP4 (intake air) uses an **Analog Input** cluster rather than Temperature Measurement so HA can
give it a distinct entity name from the cluster's `Description` ("Intake air temperature"). The
Temperature Measurement cluster has no name attribute, so EP2 and EP4 would otherwise both surface
as a generic "Temperature". EP2 (supply) stays on Temperature Measurement (it keeps the
stale → "unknown" sentinel; see below). The firmware sets EP4's `ApplicationType` to the BACnet
temperature group and `EngineeringUnits = 62`, which ZHA maps to °C.

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

### Analog Input percentages (EP5/EP6)

`PCT_HEAT_EXCHANGER` and `PCT_HEATING` (CS60 regs `0x00C8` / `0x00C9`) are exposed as two
**Analog Input (Basic) `0x000C`** endpoints, one per value, carrying `PresentValue` (float)
with `EngineeringUnits = 98` (percent). Analog Input is the only standard ZCL cluster for a
generic read-only number — there is no percentage-specific cluster (Fan Control is taken for
mode, and the measurement clusters are each tied to a physical quantity).

ZBOSS does **not** compile the Analog Input server by default (its "include all clusters" set
in `zb_vendor.h` omits it), so the firmware defines `ZB_ZCL_SUPPORT_CLUSTER_ANALOG_INPUT`
image-wide in `CMakeLists.txt` to pull the cluster source in.

To make the percentages discoverable, the firmware also sets each Analog Input cluster's
`Description` (0x001C, "Heat exchanger" / "Heating element") and `EngineeringUnits` (98 = percent) /
`ApplicationType` (percentage) attributes.

**ZHA discovery:** current ZHA (the `zha` library ≥ 1.1, HA 2024.x+) ships an *unrestricted*
`AnalogInputSensor` that auto-discovers **any** Analog Input cluster carrying a `Description`
attribute — it names the entity from that `Description` and units it from `ApplicationType` /
`EngineeringUnits`. So EP5/EP6 now appear as native `%` sensors **without a quirk** (named
"Heat exchanger" / "Heating element" from the firmware `Description`).
Older ZHA gated Analog Input to specific manufacturers (e.g. LUMI) and needed a quirk; that is no
longer the case, and defining these sensors in the quirk as well produces *duplicate* entities —
so `tools/zha-quirk/flexit-cs60-control.py` deliberately leaves EP5/EP6 to native discovery. (Z2M would still
need an external converter.)

### Analog Value writable setpoint (EP7)

The temperature setpoint (`TEMP_SETPOINT_2`, CS60 reg `0x00C2`, °C) is exposed on **EP7** as an
**Analog Value (0x000E)** `PresentValue` (float, `EngineeringUnits = 62` = °C). It is
**read-write and reportable**: the bridge pushes the live committed setpoint into it, and a write
from HA injects a setpoint change onto the RS485 bus.

- ZBOSS lacks an "Analog Output" (0x000D) server, so Analog **Value** (0x000E) is used for the
  writable channel; it carries the same `PresentValue` semantics. Like Analog Input it is not in
  ZBOSS's default cluster set, so the firmware defines `ZB_ZCL_SUPPORT_CLUSTER_ANALOG_VALUE`
  image-wide in `CMakeLists.txt`.
- **Write path:** a `PresentValue` write arrives in the device callback as raw `data32` (IEEE-754
  bits); the firmware reads it as a float, clamps to **10–30 °C**, converts to °C ×10, and queues a
  CS60 setpoint command (coil 12 / reg `0x000C` — see `flexit-cs60-communication.md` §5.4). A short
  **5 s write-hold** (`FLEXIT_SETPOINT_WRITE_HOLD_MS`) suppresses the periodic readback so the value
  doesn't snap back to the old reading before the CS60 has adopted the new one.
- **The write only sticks because the XIAO is at bus address 1** — below the CI60 panel's
  potentiometer at addr 2. The CS60 lets the *lowest* bus address own the setpoint; from a higher
  address the write is read but then restamped with the pot's value. See
  `flexit-cs60-communication.md` §5.4. Consequence: after a remote write the CI60's physical dial is
  out of sync with the actual setpoint (inherent to overriding a potentiometer); turning the dial
  still works since the XIAO only asserts coil 12 when a write is queued.
- **ZHA caveat:** ZHA has no native discovery for Analog Value (unlike Analog Input — see EP5/EP6),
  so EP7 needs the quirk. `tools/zha-quirk/flexit-cs60-control.py` maps EP7 `PresentValue` to a `.number()`
  entity (10–30 °C, 0.5 °C step). This is now the *only* thing the quirk defines.

### Attribute reporting

Configure reporting on `MeasuredValue` (temps), `FanMode` (mode), and `PresentValue` (the
EP5/EP6 percentages and the EP7 setpoint) — min/max interval + reportable change — so HA receives
push updates instead of polling. For the quirk-defined EP7 setpoint Number, ZHA sets this up from
the `reporting_config` in `tools/zha-quirk/flexit-cs60-control.py`; for the natively-discovered EP5/EP6
Analog Input sensors, ZHA configures reporting from its own defaults.

---

## Operational runbook

The XIAO runs Zigbee (to HA) and BLE (service port)
concurrently. BLE is for the development only and never required for normal HA use.

### Pair / join HA (ZHA)
1. In HA: ZHA → *Add device* (opens permit-join).
2. Reset XIAO zigbee with `ble-client zbreset`. (Leaves the network, clears ZBOSS NVRAM, reboots into
   `DEVICE_FIRST_START` → BDB network steering). The BLE link drops for ~1–5 s as it reboots.

### Re-join after a DFU
A BLE DFU (`ble-client flash …` → `confirm`) reboots the device as `DEVICE_REBOOT` with **NVRAM
intact**, so it resumes the persisted ZHA network automatically — no re-pairing. The Zigbee link
drops only for the reboot (~10 s). 


### Debug via BLE (no HA needed)
From `tools/ble-client` (pairs automatically, fixed passkey `444999`):
- `ble-client scan` — confirm the device is advertising + RSSI.
- `ble-client state` / `state --human-friendly` — decoded panel snapshot over NUS.
- `ble-client stream out.bin` — live RS485 byte capture (`xxd out.bin` to inspect).
- `ble-client mode N` — queue a mode change (0=Stop 1=Min 2=Normal 3=Max) via the Modbus slave.
- `ble-client setpoint C` — queue a temperature-setpoint change (°C, e.g. `21` or `20.5`) via the
  Modbus slave (coil 12 / reg `0x000C`). Kept for protocol RE/debugging alongside the HA path.
- `ble-client list` — MCUboot slot 0/1 image hashes.

### Availability & stale data in HA
- **Bridge or mesh down** (XIAO unplugged, out of range, or radio wedged): ZHA marks the whole
  device *unavailable* via its own availability tracking for mains-powered (rx-on) nodes — all
  entities go unavailable. No firmware action needed.
- **Data source stale** (XIAO online on Zigbee, but the RS485/CS60 bus has gone silent): the EP2
  supply-air **Temperature Measurement** channel publishes the ZCL invalid sentinel `0x8000` after
  60 s (`FLEXIT_TEMP_STALE_MS`) of no fresh reading, so HA shows it as *unknown* rather than a
  frozen value; it recovers automatically when the bus resumes. The Analog Input / Analog Value
  endpoints (EP4 intake temp, EP5/EP6 percentages, EP7 setpoint) and FanMode have no ZCL invalid
  value, so a stale reading on those holds its last-known value.

---

## References

- `flexit-cs60-communication.md` — reverse-engineered RS485/Modbus protocol.
- `tools/ble-client` — existing BLE tooling (stream/state/mode/setpoint/flash).
- `tools/zha-quirk/flexit-cs60-control.py` — ZHA v2 quirk exposing the EP7 Analog Value setpoint as a °C
  `number` (ZHA has no native Analog Value discovery). EP5/EP6 are left to ZHA's native Analog
  Input discovery (defining them in the quirk too would duplicate them). Drop into HA's
  `custom_quirks_path`; install notes are in the file's docstring.
- `tools/zb-coordinator` — test Zigbee coordinator (ncs-zigbee `network_coordinator` + a static
  PM file) for the nrf52840dk; used to verify the end-device join. Build with
  `-DZEPHYR_EXTRA_MODULES=$HOME/ncs/v3.3.0/ncs-zigbee` (after `--`), flash with `west flash`.
  Note: this replaces the `hci_usb` BLE adapter on the DK, so `ble-client` is offline while the
  coordinator runs (restore with `west flash --build-dir build-hci-usb`).
- `tools/zb-shell` — ZCL-capable bench coordinator (ncs-zigbee `shell` sample + static PM) for the
  nrf52840dk. Resumes the persisted network from NVRAM (`bdb role zc` / `bdb start`); drive ZCL over
  `/dev/ttyACM0` (`zcl attr read/write`, `zdo bind`, `zcl subscribe`). 
- HA ZHA integration: https://www.home-assistant.io/integrations/zha/
