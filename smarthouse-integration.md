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

| Transport | Role | Audience |
|-----------|------|----------|
| **Zigbee** (802.15.4) | Production smarthouse integration — always-on end device exposing temps + mode | Home Assistant |
| **BLE** | Debug/diagnostics (RS485 stream, state dump) **and firmware flashing (SMP DFU)** only | Developer, on demand |

---


## Zigbee clusters / endpoints

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

### Attribute reporting

Configure reporting on `MeasuredValue` and `FanMode` (min/max interval + reportable
change) so HA receives push updates instead of polling.

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
- `ble-client list` — MCUboot slot 0/1 image hashes.

### Availability & stale data in HA
- **Bridge or mesh down** (XIAO unplugged, out of range, or radio wedged): ZHA marks the whole
  device *unavailable* via its own availability tracking for mains-powered (rx-on) nodes — all
  entities go unavailable. No firmware action needed.
- **Data source stale** (XIAO online on Zigbee, but the RS485/CS60 bus has gone silent): each
  temperature channel publishes the ZCL invalid sentinel `0x8000` after 60 s
  (`FLEXIT_TEMP_STALE_MS`) of no fresh reading, so HA shows those sensors as *unknown* rather than a
  frozen value. They recover to live readings automatically when the bus resumes. FanMode has no
  ZCL invalid value, so a stale mode holds its last-known reading.

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
- HA ZHA integration: https://www.home-assistant.io/integrations/zha/
