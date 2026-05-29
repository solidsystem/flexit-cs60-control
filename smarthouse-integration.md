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

### Why this split

- **Range is the deciding factor.** The CS60 sits in a technical room, often a room or
  two away from the HA host. BLE through walls is marginal here — we already hit
  `le-connection-abort-by-local` below ~-75 dBm and have to restart BlueZ to recover.
  A BLE-only HA integration would inherit that fragility in the production path.
- **Zigbee fixes range via mesh.** Mains-powered Zigbee routers already in the house
  (smart plugs, bulbs) relay traffic, so the bridge's placement is far less of a range
  problem. An always-on (rx-on-when-idle) end device gives prompt mode changes and
  push attribute reporting.
- **Native HA entities, little glue code.** Standard Zigbee clusters (Temperature
  Measurement, Fan Control) auto-discover in ZHA/Zigbee2MQTT as proper sensor/fan
  entities — far less HA-side code than a custom `bleak` integration for a bespoke GATT
  service would have needed.
- **Keep BLE because it already works.** The transport, SMP DFU, and tooling exist
  today. Retiring it would mean either USB-cable-only updates or implementing Zigbee
  OTA. Keeping BLE for debug + DFU sidesteps **Zigbee OTA entirely** — the single
  biggest simplification this split buys us.
- **Transport-agnostic core.** The RS485 → decoded-state logic is shared; only the
  layer that publishes state and accepts a mode command differs per radio.

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

> ✅ **RESOLVED in [Phase 0](#phase-0-results-done) — the add-on supports v3.3.0.**
> Zigbee (ZBOSS, Zigbee R22) was deprecated in NCS **v2.8.0** and **removed from the
> main SDK in v3.0.0**. It now lives as a separate out-of-tree **add-on**
> (`nrfconnect/ncs-zigbee`). The *tagged* releases (≤ v1.3.0) pin sdk-nrf **v2.9.2**,
> which is why the releases page looked incompatible — but the add-on's **`main` branch
> pins sdk-nrf `v3.3.0`** (its `west.yml`), and a Zigbee End Device sample **builds
> cleanly against our installed v3.3.0** (see Phase 0 results). Use the `main` branch
> (or pin a specific commit) rather than a tag.

### Other constraints to validate

- **SMP-over-BLE + Zigbee together.** *Compile proven in Phase 0:* the
  `overlay-multiprotocol_ble.conf` build enables `CONFIG_BT` + `CONFIG_BT_SMP` + NUS +
  bonding **alongside** the Zigbee End Device and links fine. Our firmware's BLE surface
  is exactly those two standard services — **NUS** (diagnostic channel) + **SMP** (DFU);
  there is no custom GATT service. *Runtime* coexistence (a live BLE DFU while joined to
  the Zigbee mesh) still needs on-hardware validation.
- **Partition layout.** The MCUboot DFU secondary slot lives in the XIAO BLE's onboard
  2 MB QSPI flash, leaving a single ~916 KB primary slot in the internal flash. See the
  Phase 1 results for the applied layout.
- **Partition Manager.** v3.3.0 emits a *deprecation warning* for `PARTITION_MANAGER`,
  but it still works and the add-on depends on it (`select PM_SINGLE_IMAGE`, ZBOSS
  partitions declared via Kconfig). Watch for its removal in a future NCS.
- **Module registration matters for partitions.** The ZBOSS partitions are only
  auto-placed when ncs-zigbee is a **real west module** (in the manifest), so sysbuild's
  Partition Manager discovers them. Injecting it ad-hoc via `ZEPHYR_EXTRA_MODULES`
  compiles the code but skips partition placement — then a **static** `pm_static.yml` is
  required (as done in the spike).
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

1. ~~**[BLOCKER] ncs-zigbee add-on vs NCS v3.3.0.**~~ **RESOLVED in Phase 0** — `main`
   branch targets v3.3.0 and builds for `xiao_ble/nrf52840`.
2. **SMP DFU + Zigbee runtime coexistence.** Compile is proven; a *live* BLE DFU while
   joined to the Zigbee mesh still needs on-hardware validation.
3. **Production partition layout** — add MCUboot dual-slot to the validated spike layout
   without colliding with ZBOSS NVRAM / settings.
4. **Mode write → CS60 ack timing** over Zigbee feels responsive (FC65 round-trip).
5. **ZHA quirk / Z2M converter** needed for the Fan Control mode mapping, or does it
   auto-discover cleanly?

---

## Phase 0 results (DONE)

Spike performed 2026-05-28 against the installed NCS **v3.3.0** toolchain, using the
**`main`** branch of `nrfconnect/ncs-zigbee` injected via `ZEPHYR_EXTRA_MODULES` plus a
hand-written static partition file. **Conclusion: the dual-protocol approach is viable
on v3.3.0 — no blocker.**

| Build (sample: `light_switch`, role: **End Device**) | Board | Result | FLASH | RAM |
|---|---|---|---|---|
| Zigbee only | `nrf52840dk/nrf52840` | ✅ links | 33.5 % | 17.3 % |
| Zigbee + BLE (`overlay-multiprotocol_ble.conf`: BT+SMP+NUS+bonding) | `nrf52840dk/nrf52840` | ✅ links | 46.7 % | 24.0 % |
| Zigbee only | **`xiao_ble/nrf52840`** (our target) | ✅ links | 35.9 % | 20.5 % |

Findings:

- **v3.3.0 compatible:** add-on `main` `west.yml` pins `sdk-nrf` `v3.3.0`; tagged
  releases (≤ v1.3.0) still pin v2.9.2 — so **track `main`/a commit, not a tag**.
- **Everything compiles:** ZBOSS stack, `nrf_802154` driver, `zigbee_app_utils`, and
  the End Device sample all build with the v3.3.0 toolchain.
- **Comfortable footprint:** even BLE+Zigbee together leaves ~53 % flash / ~76 % RAM
  free — room for MCUboot dual-slot + our app.
- **Partition placement requires real module registration.** With `ZEPHYR_EXTRA_MODULES`
  the code compiles but sysbuild's Partition Manager doesn't place the ZBOSS partitions
  (`PM_ZBOSS_NVRAM_*` undeclared). Fixed in the spike with a static `pm_static.yml`;
  production should register ncs-zigbee in the **west manifest** so PM auto-places them.
- **Deprecation watch:** `PARTITION_MANAGER` is deprecated in v3.3.0 (warning only).

Validated single-image spike layout (nRF52840, **no MCUboot** — production must insert
MCUboot slots):

```yaml
app:                   { address: 0x0,     size: 0xf6000 }
settings_storage:      { address: 0xf6000, size: 0x1000 }   # BLE bonding (multiprotocol)
zboss_product_config:  { address: 0xf7000, size: 0x1000 }
zboss_nvram:           { address: 0xf8000, size: 0x8000 }
```

## Phase 1 results (DONE — builds; runtime unverified)

Performed 2026-05-28. The **real flexitMC firmware** now builds as a dual-protocol image
(BLE for debug/DFU + Zigbee end device) for `xiao_ble/nrf52840` with MCUboot dual-slot.

What changed in the project:

- `CMakeLists.txt` — registers the add-on via `ZEPHYR_EXTRA_MODULES`
  (`~/ncs/v3.3.0/ncs-zigbee`, commit `8a6c6ca`); adds `src/zigbee_ep.c`.
- `sysbuild.conf` — `SB_CONFIG_PARTITION_MANAGER=y` (the add-on requires PM; the project
  previously used devicetree partitions) + `SB_CONFIG_PM_EXTERNAL_FLASH_MCUBOOT_SECONDARY=y`.
- `pm_static_xiao_ble_nrf52840.yml` — new static layout (below): big internal primary
  slot, DFU **secondary slot in the onboard QSPI flash**.
- `prj.conf` — `CONFIG_ZIGBEE_ADD_ON/APP_UTILS/ROLE_END_DEVICE=y` + `CONFIG_NORDIC_QSPI_NOR=y`.
  Logging and NUS are both retained.
- `boards/xiao_ble_nrf52840.overlay` + `sysbuild/mcuboot.overlay` — `chosen
  nordic,pm-ext-flash = &p25q16h`. `sysbuild/mcuboot.conf` — QSPI driver + `BOOT_MAX_IMG_SECTORS=256`.
- `src/zigbee_ep.{c,h}` — HA **Temperature Sensor** end device (Basic + Identify +
  Temperature Measurement) + `zboss_signal_handler`; started from `main()` after BLE.

Final footprint (sysbuild, green):

| Image | FLASH used | of | RAM |
|---|---|---|---|
| app (flexitMC) | 496 880 B | 937 472 B slot (**53.0 %**) | 109 KB / 256 KB (41.5 %) |
| MCUboot | 34 968 B | 64 KB (53.4 %) | — |

Both DFU artifacts are produced: `build/flexitMC/zephyr/zephyr.signed.bin` (BLE DFU) and
`build/dfu_application.zip` (USB DFU). The temperature endpoint is a real, joinable HA
device; Fan Control (mode) and live CS60 data come in Phase 2.

The MCUboot DFU **secondary (staging) slot lives in the XIAO BLE's onboard 2 MB QSPI flash**
(`p25q16h`), so the internal flash carries only the bootloader, a single ~916 KB primary
slot, and the persistent areas. This leaves logging + NUS enabled and ZBOSS NVRAM at 32 KB,
with ~440 KB of the primary slot free for Phase 2.

Applied production partition layout (`pm_static_xiao_ble_nrf52840.yml`, swap-using-move,
secondary in QSPI):

```yaml
# internal flash (1 MB)
mcuboot:               { address: 0x00000, size: 0x10000 }
mcuboot_primary:       { address: 0x10000, size: 0xe5000 }   # = mcuboot_pad + app (~916 KB)
settings_storage:      { address: 0xf5000, size: 0x02000 }   # BLE bonding (NVS)
zboss_product_config:  { address: 0xf7000, size: 0x01000 }
zboss_nvram:           { address: 0xf8000, size: 0x08000 }   # 32 KB
# external QSPI flash (2 MB), region external_flash
mcuboot_secondary:     { address: 0x00000, size: 0xe5000 }   # DFU staging
external_flash:        { address: 0xe5000, size: 0x11b000 }  # spare
```

Still unverified (needs hardware + a coordinator): actual network join, mode/temperature
round-trips, a BLE DFU performed while joined to the Zigbee mesh, and — new with this
layout — that **MCUboot swaps correctly from the QSPI secondary** (the upload writes to
external flash, then the bootloader copies it into the internal primary on reboot).

> **Note on module registration:** Phase 1 uses `ZEPHYR_EXTRA_MODULES` + a static
> `pm_static.yml` (PM places the partitions from the static file, so the Phase-0 caveat
> about manifest registration doesn't block us). A west-manifest-based setup is still the
> cleaner long-term option but is not required to build.

## Hardware verification (2026-05-29 — BLE path confirmed on device)

First on-target flash of the dual-protocol image. **Key constraint:** the Phase 1 layout
changes the bootloader region (`0xc000`→`0x10000`), adds the QSPI secondary slot, and
relinks the app at the new primary — so the first install **cannot** be done with the
normal app-only DFU paths (SMP-over-USB `mcu-manager`, or BLE `ble-client flash`); those
only write the app slot through the *running* MCUboot. The XIAO's UF2 bootloader is not
present either, so the one-time install was done over **SWD/J-Link** (the nRF52840DK's
onboard J-Link wired to the XIAO's SWD pads):

```bash
# nrfutil device (this host has no separate nrfjprog)
nrfutil device read    --address 0x0 --bytes 32        # confirm target = XIAO, not the DK chip
nrfutil device program --firmware build/merged.hex \
        --options chip_erase_mode=ERASE_ALL,verify=VERIFY_READ,reset=RESET_SYSTEM
```

Pre-flash read confirmed the target was the XIAO (MCUboot image header at the *old* `0xc000`),
APPROTECT was open (no `--recover` needed), and post-flash read-back confirmed the new layout
(image header now at `0x10000`, `img_size 0x794F0` ≈ matches `zephyr.signed.bin`). After this
one SWD flash, normal BLE/USB DFU works again because the QSPI secondary slot now exists.

Verified working on hardware (RS485 bus **not** connected during this test):

- ✅ Boots and runs from the new layout; no ZBOSS assert-reboot loop.
- ✅ USB CDC console enumerates (`2fe3:0004`, `/dev/ttyACM2`).
- ✅ BLE advertises as `flexitMC3`.
- ✅ Pairing/security L3 with fixed passkey `444999` (old bond invalidated by the
      `settings_storage` move, as expected — re-paired with fresh keys).
- ✅ NUS round-trip: `ble-client state` returned a decoded snapshot line.
- ✅ **BLE + Zigbee coexist at runtime** — the BLE connect/pair/NUS all succeeded while
      ZBOSS was concurrently running network steering (the headline multiprotocol risk held).

`state` returned all-zero / `n/a` fields (`CRCERR=0`, all FC counters `0`) — correct, since
RS485 was disconnected (no CS60 traffic to decode). Zigbee join was intentionally not tested;
the device is steering on channel 16 and failing to join with no coordinator present (expected).

> **Gotcha that cost time:** the BLE HCI adapter used for testing (the DK's `hci_usb`,
> `2fe3:000b`) wedged during the SWD session — `hciconfig hci0` showed BD address
> `00:00:…` with I/O errors and scans returned nothing. A `systemctl restart bluetooth`
> did **not** fix it; only **unplugging/replugging the DK's nRF-USB cable** (power-cycling
> the hci_usb firmware) restored it. Symptom looked like "XIAO not advertising" but was the
> host adapter, not the firmware.

Still pending hardware verification: RS485 decode into `state` (reconnect the bus),
Zigbee network join, and a BLE/USB DFU swap from the QSPI secondary.

## TODO

### Phase 0 — De-risk the toolchain ✅ DONE (see results above)
- [x] Build a Zigbee End Device sample on v3.3.0 — builds for `nrf52840dk` **and**
      `xiao_ble/nrf52840`.
- [x] Decide version strategy — **no older-NCS pin needed**; track ncs-zigbee `main`.
- [x] Build the `light_switch` multiprotocol overlay — concurrent BLE + Zigbee links.

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
- Reference project: https://github.com/MSkjel/esphome-flexit-modbus-server
- ncs-zigbee add-on: https://github.com/nrfconnect/ncs-zigbee
- Zigbee light_switch (multiprotocol BLE overlay): https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/samples/zigbee/light_switch/README.html
- HA ZHA integration: https://www.home-assistant.io/integrations/zha/

### Sources for the feasibility findings

- [nRF52840 product page — full protocol concurrency](https://www.nordicsemi.com/Products/nRF52840)
- [Multiprotocol support (MPSL / dynamic concurrency)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/1.4.2/nrf/ug_multiprotocol_support.html)
- [NCS v3.0.0 release notes — Zigbee moved to add-on](https://docs.nordicsemi.com/bundle/ncs-3.0.0/page/nrf/releases_and_maturity/releases/release-notes-3.0.0.html)
- [ncs-zigbee releases (NCS compatibility)](https://github.com/nrfconnect/ncs-zigbee/releases)
