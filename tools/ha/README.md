# Home Assistant integration — flexitMC (Phase 3)

The XIAO presents as a **single Zigbee node** (one HA *device*) with several
entities. The temperatures sit on separate endpoints only because the ZCL
Temperature Measurement cluster carries a single value — they are still one
device in HA.

## Zigbee signature

- **Manufacturer (Basic 0x0004):** `SolidSystem`
- **Model (Basic 0x0005):** `flexitMC`

| Endpoint | Profile | Device type | Input (server) clusters | Maps to | HA entity |
|----------|---------|-------------|-------------------------|---------|-----------|
| 1 | 0x0104 (HA) | 0x0301 Thermostat | 0x0000 Basic, 0x0003 Identify, **0x0202 Fan Control** | mode (read + write) | `fan` |
| 2 | 0x0104 | 0x0302 Temp Sensor | **0x0402 Temperature Measurement** | supply air | `sensor` |
| 3 | 0x0104 | 0x0302 Temp Sensor | 0x0402 Temperature Measurement | extract air | `sensor` |
| 4 | 0x0104 | 0x0302 Temp Sensor | 0x0402 Temperature Measurement | outdoor air | `sensor` |

Each Temperature Measurement cluster (EP2/3/4) also carries a custom **read-only
label attribute `0xF000`** (ZCL character string) returning `supply` / `extract` /
`outdoor`, so an endpoint can be identified by reading one attribute. It is not
manufacturer-coded (read it with no manufacturer code) and ZHA/Z2M ignore it during
interview — it creates no entity. Read it via ZHA → *Manage Zigbee device* → the
endpoint's Temperature Measurement cluster → read attribute `0xF000` (61440), or
`zcl attr read <addr> 2 0402 0104 f000` on the bench shell.

**Fan Control `FanMode` (0x0202 / attr 0x0000, enum8, rw, reportable):**

| FanMode | Flexit mode |
|---------|-------------|
| 0 Off    | Stop   |
| 1 Low    | Min    |
| 2 Medium | Normal |
| 3 High   | Max    |

The device works with no quirk/converter: ZHA exposes one `fan` + three
`temperature` sensors (named generically "Temperature", "Temperature 2/3").

**ZHA — rename in the UI, do not use a quirk.** Every entity here is a *standard*
cluster sensor. A zigpy v2 `QuirkBuilder.sensor()` only *adds* an entity; it does
not replace ZHA's auto-created one, so a naming quirk produces **duplicate**
temperature entities (one generic + one custom). There is no clean, version-stable
way to suppress the default. So for ZHA just rename the three temperature entities
in the UI (device → entity → ✏️). Those renames are stored in the entity registry
by `unique_id` (IEEE + endpoint + cluster), so they **survive restarts and re-pairs**
(including `zbreset`). A ZHA quirk was tried and removed for this reason.

The `zigbee2mqtt_flexitmc.js` external converter is kept for **Z2M** users (Z2M
converters replace exposes cleanly, so no duplicate issue there) — still untested.

## Expected entities (one device)

- `fan.flexitmc_*` — Off / Low / Medium / High (→ Stop / Min / Normal / Max)
- three `sensor.flexitmc_temperature*` on endpoints 2/3/4 — supply / extract /
  outdoor air. ZHA names them generically; rename in the UI to taste (the
  endpoint→meaning mapping is the table above: EP2 supply, EP3 extract, EP4 outdoor).

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
3. ZHA: nothing to install — just rename the three temperature entities in the
   UI (see above). Z2M: point `external_converters` at `zigbee2mqtt_flexitmc.js`.
4. Confirm the device shows one card with the fan + three temperature sensors,
   that reads work, that setting the fan changes mode, and that temperatures
   update via **reporting** (ZHA/Z2M bind the clusters automatically — that
   binding is what makes reports flow; verified on the bench with a manual
   `zdo bind`, see smarthouse-integration.md Phase 2).

## Bench note (no HA required)

The protocol-level data path is already verified with `tools/zb-shell`:
reads, a `FanMode` write, and — after a manual `zdo bind` — live attribute
reports. ZHA/Z2M automate that binding.

ZHA pairing is **verified on real HA** (2026-05-30, HA 2026.4.x): the device joins
and auto-discovers one fan + three temperature sensors; friendly names done by UI
rename. The `zigbee2mqtt_flexitmc.js` converter remains **untested against a live
Z2M** and may need tweaks for your Z2M version.
