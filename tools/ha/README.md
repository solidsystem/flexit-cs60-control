# Home Assistant integration — flexitMC3 (Phase 3)

The XIAO presents as a **single Zigbee node** (one HA *device*) with several
entities. The temperatures sit on separate endpoints only because the ZCL
Temperature Measurement cluster carries a single value — they are still one
device in HA.

## Zigbee signature

- **Manufacturer (Basic 0x0004):** `SolidSystem`
- **Model (Basic 0x0005):** `flexitMC3`

| Endpoint | Profile | Device type | Input (server) clusters | Maps to | HA entity |
|----------|---------|-------------|-------------------------|---------|-----------|
| 1 | 0x0104 (HA) | 0x0301 Thermostat | 0x0000 Basic, 0x0003 Identify, **0x0202 Fan Control** | mode (read + write) | `fan` |
| 2 | 0x0104 | 0x0302 Temp Sensor | **0x0402 Temperature Measurement** | supply air | `sensor` |
| 3 | 0x0104 | 0x0302 Temp Sensor | 0x0402 Temperature Measurement | extract air | `sensor` |
| 4 | 0x0104 | 0x0302 Temp Sensor | 0x0402 Temperature Measurement | outdoor air | `sensor` |

**Fan Control `FanMode` (0x0202 / attr 0x0000, enum8, rw, reportable):**

| FanMode | Flexit mode |
|---------|-------------|
| 0 Off    | Stop   |
| 1 Low    | Min    |
| 2 Medium | Normal |
| 3 High   | Max    |

Without any quirk/converter the device already works: ZHA exposes one `fan` +
three `temperature` sensors (named generically "Temperature", "Temperature 2/3").
The quirk/converter here only add the friendly **supply/extract/outdoor** names.

## Expected entities (one device)

- `fan.flexitmc3_*` — Off / Low / Medium / High (→ Stop / Min / Normal / Max)
- `sensor.flexitmc3_supply_air_temperature`
- `sensor.flexitmc3_extract_air_temperature`
- `sensor.flexitmc3_outdoor_air_temperature`

## Pairing & validation runbook

A Nordic ZBOSS device needs a coordinator ZHA/Z2M can drive. Options:

1. **ZHA + `zigpy-zboss`** with the DK flashed as a Zigbee **NCP**
   (`ncs-zigbee/samples/ncp`, the same static-PM treatment as `tools/zb-shell`).
2. **Z2M** likewise via a zboss-capable adapter, or any spare Silabs/TI
   coordinator if you have one.

Steps:

1. Flash the XIAO with the current firmware (already done) — manufacturer/model
   are baked into the Basic cluster.
2. Bring up the coordinator in HA, put it in "permit join", and reset/he-join
   the XIAO so it joins the ZHA/Z2M network (it multi-channel scans, so no fixed
   channel is needed). NOTE: joining a new coordinator means leaving the bench
   `0x4716` network; factory-reset the XIAO's Zigbee state if it won't re-steer.
3. ZHA: drop `zha_quirk_flexitmc.py` into your `zha_quirks`/custom-quirks path.
   Z2M: point `external_converters` at `zigbee2mqtt_flexitmc.js`.
4. Confirm the device shows one card with the fan + three temperature sensors,
   that reads work, that setting the fan changes mode, and that temperatures
   update via **reporting** (ZHA/Z2M bind the clusters automatically — that
   binding is what makes reports flow; verified on the bench with a manual
   `zdo bind`, see smarthouse-integration.md Phase 2).

## Bench note (no HA required)

The protocol-level data path is already verified with `tools/zb-shell`:
reads, a `FanMode` write, and — after a manual `zdo bind` — live attribute
reports. ZHA/Z2M automate that binding. These two files are **untested against a
live HA/Z2M** and may need small tweaks for your exact ZHA (zigpy) / Z2M version.
