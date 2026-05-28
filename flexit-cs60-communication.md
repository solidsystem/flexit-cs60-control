# Flexit CS60 communication — RS485 protocol observations

Notes on how the Flexit **CS60** ventilation controller talks to its
**CI60** panel (and to other slaves) over the RJ12-cabled RS485 bus.
All observations come from passive sniffing of the live bus, captured
with a DMA-driven RS485 listener and recorded to file for offline
analysis.

Four captures are referenced:

| File                                       | Size      | Duration | Notes |
|--------------------------------------------|-----------|----------|-------|
| `trafficdata/rs485-idle.bin`               |  41 770 B | ~18 s    | Idle baseline — no panel interaction. CI60 only. |
| `trafficdata/rs485-panel-up-down.bin`      | 130 260 B | ~54 s    | Speed setting changed via panel: min → medium → max → medium → min. CI60 only. |
| `trafficdata/rs485-init-2.bin`             | ~80 kB    | varies   | CS60 cold-boot with sniffer up but no extra slave answering FC04. |
| `trafficdata/rs485-init-3.bin`             | ~76 kB    | varies   | CS60 cold-boot with a second slave answering FC04 (TX echo into the capture). |

The idle and panel captures show ~16–17 % bus utilisation at 115200
baud during steady-state polling (~8 cycles/s). The init captures
cover the much rarer boot transient (§6).

---

## 1. Bus parameters

| Parameter | Value | How verified |
|-----------|-------|-------------|
| Baud rate | **115200 / 8N1** | Frames decode cleanly. Matches the [ESPHome Flexit-Modbus-Server](https://github.com/MSkjel/esphome-flexit-modbus-server) reference. |
| Wiring    | Half-duplex RS485 | Master and slave traffic share one differential pair. |
| Bus master | Flexit **CS60** | All polling traffic originates from one party. |
| Polled slaves | Determined at CS60 boot via FC04 enumeration (§6) | Each registered address gets the full FC01 poll pair every cycle. |
| Broadcasts | addr `0x00` | All FC06, FC10 and FC65 use addr 0 (Modbus broadcast). |

The CS60 is the only master. Slaves only respond — never initiate
unsolicited traffic. Broadcasts are not echoed by slaves.

> **Slave set is fixed at boot.** The CS60 sweeps addresses 0x01..0x03
> (plus 0xFE) with FC04 ReadInputRegs once at power-up; whoever
> answers becomes a polled slave for the rest of the session. Flipping
> the CI60 panel's DIP-switch from off to on moves it from addr 2 to
> addr 3, but only takes effect after the next CS60 power-cycle.
> Hot-plugging a slave at runtime is invisible — it has to be present
> and answering FC04 *when* CS60 boots. See §6 for the full
> enumeration sequence.

> The CS60 *intentionally ignores* Modbus-RTU inter-frame timing — see
> `setup()` in MSkjel's [`flexit_modbus_server.cpp`](https://github.com/MSkjel/esphome-flexit-modbus-server/blob/main/flexit_modbus_server.cpp):
> *"The CS/CU/CE60 doesnt actually follow the Modbus RTU spec. It just
> ignores any interframe timeout and blasts request."*
> Receivers therefore have to be DMA-driven or otherwise tolerant of
> zero-gap framing: the master will happily send a second frame the
> byte after the first ends.

---

## 2. Polling cadence (steady state)

```
~120 ms cycle, repeated continuously (per registered slave):
  ┌──────────────────────────────────────────────────────────┐
  │  FC10 broadcast (179 B)         status block 0x00BE+85   │
  │  FC01 req A (8 B)               read coils 0..331         │
  │  FC01 resp A (47 B)             332-coil bitmap            │
  │  FC01 req B (8 B)               read coils 332..683        │
  │  FC01 resp B (49 B)             352-coil bitmap            │
  │  occasionally FC06 broadcasts   runtime counters           │
  └──────────────────────────────────────────────────────────┘
  ≈ 290 bytes / cycle ⇒ ~8 cycles / s at 2 400 B/s
```

In the panel capture (single registered slave = CI60) the CS60 issues
**450 FC10 broadcasts in 54 s** (8.3 broadcasts / s) and **1 626 FC01
requests** (30 req/s = 8.3 request *pairs* / s — matches). FC06
broadcasts run at ~3.7 / s. Every fan-speed change carries through to
slaves within 120 ms.

When two slaves are registered, the CS60 fires the FC01 pair at each
registered address back-to-back with no idle gap between them. Each
slave gets the same ~8 polls / s, total FC01 traffic roughly doubles.
The FC10 broadcast cadence does **not** double; it stays at ~8 / s
regardless of slave count.

---

## 3. Function codes observed

A length-ascending CRC scan over the steady-state captures finds the
following function codes — and **only** these:

| FC | Idle capture | Panel capture |
|----|-------------:|--------------:|
| `0x01` ReadCoils                                | 514 | 1 626 |
| `0x03` ReadHoldingRegs                          |   0 |     6 |
| `0x04` ReadInputRegs *(boot only — see §6)*     |   0 |     0 |
| `0x06` WriteSingleReg                           |  67 |   203 |
| `0x10` WriteMultipleRegs                        | 146 |   450 |
| `0x65` Flexit reset                             |   0 |     3 |
| `0x00` (false-positive CRC coincidences)        |   0 |     3 |

FC02 (ReadDiscreteInputs), FC05 (WriteSingleCoil), FC0F
(WriteMultipleCoils), FC11 (ReportSlaveID) and FC17
(ReadWriteMultipleRegs) are absent from all captures. FC04 appears
only during boot enumeration (§6); it never shows up in steady state.

The three `fc=0x00` hits in the panel capture are CRC coincidences
inside corrupted long frames (lengths 106, 159, 195 — not matching
any real Modbus frame shape).

| FC | Name | Direction | What it carries |
|----|------|-----------|-----------------|
| `0x01` | ReadCoils | CS60 → slave | Polls the slave's "pending command" coil bitmap. Two requests per cycle cover coils 0–683. Responses are 332-coil (47 B) and 352-coil (49 B). The bitmap is **all zeros** until a slave raises a command flag (coil 0 = CMD_MODE in the captured cases). |
| `0x03` | ReadHoldingRegs | CS60 → slave | Single-register reads, only issued *as a follow-up* after a slave has raised a coil. The request is 8 B (`02 03 00 00 00 01 84 39` = read 1 reg at addr 0); the response is 7 B (`02 03 02 NN NN CC CC`). |
| `0x04` | ReadInputRegs | CS60 → slave (probe) | Boot-time enumeration sweep — 8 B request asking for input regs 0..3. See §6. |
| `0x06` | WriteSingleReg | CS60 → broadcast | Runtime counters (operating-minutes per mode + filter + rotor). 67 frames in 18 s idle, 203 in 54 s panel. |
| `0x10` | WriteMultipleRegs | CS60 → broadcast | Two distinct shapes: (a) the **status block** at `0x00BE` (85 regs, 179 B, every ~120 ms), and (b) a **bulk-init burst** that appears only at boot — four chained writes covering regs `0x0000..0x015F`, see §6. |
| `0x65` | Flexit proprietary "reset coil+register" | CS60 → broadcast | 8-byte frame `00 65 AH AL VH VL CC CC`. [ESPHome](https://github.com/MSkjel/esphome-flexit-modbus-server) handles it as `setHoldingRegister(addr, value); setCoil(addr, 0);` — clears the coil that the slave set, while stamping the register with the broadcast value. Once per command cycle, always paired with an FC03 read. |

---

## 4. Idle baseline (`rs485-idle.bin`, ~18 s)

Frame coverage: **96.1 %** valid CRC.

| Metric | Value |
|--------|-------|
| Capture size | 41 770 B |
| Valid frames | 727 |
| FC01 ReadCoils (addr 2) | 514 |
| FC10 status broadcasts | 146 |
| FC06 runtime counters | 67 |
| FC03 / FC65 | 0 / 0 |

The status broadcast block, decoded:

| Reg | Name | Value seen | Notes |
|-----|------|------------|-------|
| `0x00BE` | TEMP_SETPOINT      | 19.6 °C | constant |
| `0x00BF` | MODE               | 1 (Min) | constant — no buttons pressed |
| `0x00C0` | UNKNOWN_1          | `0x0DA6` = 3494 | constant in capture; possibly a counter tied to a slower clock |
| `0x00C1` | UNKNOWN_2          | 0 | constant |
| `0x00C2` | TEMP_SETPOINT_2    | 19.6 °C | constant |
| `0x00C3` | TEMP_SUPPLY_AIR    | 20.0 °C | actual supply-air sensor |
| `0x00C4` | TEMP_EXTRACT_AIR   | -187.5 / -125.0 °C | **sensor-not-present sentinel** (no extract probe wired) |
| `0x00C5` | TEMP_OUTDOOR_AIR   | 14.6 °C | actual outdoor sensor |
| `0x00C6` | TEMP_RETURN_WATER  | -250.0 / -187.5 °C | **sensor-not-present sentinel** (no water-coil sensor) |
| `0x00C7` | PCT_COOLING        | 0 % | no cooling stage |
| `0x00C8` | PCT_HEAT_EXCHANGER | 14–16 % | rotary HX modulation |
| `0x00C9` | PCT_HEATING        | 0 % | no heater output |
| `0x00CA` | PCT_SUPPLY_FAN     | **50 %** | Min mode fan setpoint |
| `0x00CB` | (unnamed)          | `0xDD75` = 56 693 | constant — probably a checksum/magic |
| `0x00CC` | (unnamed)          | `0x001A` = 26 | constant |

The slave coil bitmap is **all zeros** throughout: no slave is raising
any command flag while idle. Consequently the CS60 issues no FC03
reads and no FC65 resets — confirming that the FC03 / FC65 cycle is
*command-driven*, not periodic.

---

## 5. Panel-button capture (`rs485-panel-up-down.bin`, ~54 s)

User actions: **min → medium → max → medium → min**, a few seconds
between each press.

Frame coverage: **96.5 %** valid CRC.

| Metric | Value |
|--------|-------|
| Capture size | 130 260 B |
| Valid frames | 2 291 |
| FC01 ReadCoils (addr 2) | 1 626 |
| FC10 status broadcasts | 450 |
| FC06 runtime counters | 203 |
| **FC03 ReadHoldingRegs (req+resp)** | **3 pairs = 6 frames** |
| **FC65 broadcast resets** | **3 frames** |

### 5.1 MODE timeline (from FC10 status broadcasts)

The MODE register at `0x00BF` republishes the panel's speed setting on
every status broadcast — every transition is therefore visible within
~120 ms of the button press:

```
  t=0.04s … 7.14s  MODE = 1 (Min)        PCT_FAN = 50 %    (initial state)
  t=7.25s          MODE: 1 → 2 (Normal)  PCT_FAN: 50 → 69 %   [press #1]
  t=7.25s … 19.27s MODE = 2 (Normal)
  t=19.38s         MODE: 2 → 3 (Max)     PCT_FAN: 69 → 100 %  [press #2]
  t=19.38s … 32.47s MODE = 3 (Max)
  t=32.58s         MODE: 3 → 2 (Normal)  PCT_FAN: 100 → 69 %  [press #3]
  t=32.58s … 44.28s MODE = 2 (Normal)
  t=44.39s         MODE: 2 → 1 (Min)     PCT_FAN: 69 → 50 %   [press #4]
```

Four MODE transitions for four button presses, with constant
fan-speed mapping **Min=50 % / Normal=69 % / Max=100 %**.

### 5.2 Full command cycle (per button press)

For three of the four presses the entire cycle is captured. Example
for press #2 (Normal → Max):

```
  t=18.41 s    panel FC01 resp     bitmap[0] = 0x01
                                   (coil 0 = CMD_MODE = pending command)
               raw: 02 01 2A 01 00 00 00 00 ... 00 CC CC
                    ^^ ^^ ^^ ^^
                    │  │  │  └─ coil-0 byte (bit 0 = 1)
                    │  │  └─── byte_count = 42 (332 coils / 8)
                    │  └────── FC01
                    └───────── addr 2 (panel)

  t=18.43 s    CS60 FC03 req      read 1 holding reg at addr 0
               raw: 02 03 00 00 00 01 84 39

  t=18.43 s    panel FC03 resp    byte_count=2, value = 0x0003 (Max)
               raw: 02 03 02 00 03 BC 45
                          ^^ ^^^^^
                          bc  value

  t=18.44 s    CS60 FC65 cast     addr = 0x0000, value = 0x0003
               raw: 00 65 00 00 00 03 0C 12
                    (clear coil 0, stamp holding_reg[0] = 3)

  t=19.38 s    CS60 FC10 cast     status block updated, MODE = 3 (Max)
```

Same shape for press #3 (Max → Normal) at t=31.61 s, and press #4
(Normal → Min) at t=43.54 s. Press #1 (Min → Normal at 7.25 s) shows
only the resulting FC10 broadcast — its coil/FC03/FC65 trio happened
before the capture started, but the MODE transition still appears in
the broadcast stream.

All three observed FC65 broadcasts decode as:

| t (s) | Frame                       | Decoded |
|-------|-----------------------------|---------|
| 18.44 | `00 65 00 00 00 03 0C 12`   | addr=0, value=3 (Max) |
| 31.64 | `00 65 00 00 00 02 CD D2`   | addr=0, value=2 (Normal) |
| 43.56 | `00 65 00 00 00 01 8D D3`   | addr=0, value=1 (Min) |

…each carrying the new MODE the CS60 just consumed from the panel's
command register. The address inside the FC65 frame (`0x0000`) is the
holding-register / coil index being acked — in this case `CMD_MODE`.

### 5.3 Coil bitmap content during the press

Each non-zero panel FC01 response in the capture has the **same**
bitmap: byte 0 = `0x01`, all other bytes zero. That maps to **coil 0
only** — the `CMD_MODE` flag. No other panel-driven commands fired in
this capture.

---

## 6. Boot enumeration (`rs485-init-3.bin`)

The first thing the CS60 emits after power-up is a single enumeration
sweep using **FC04 ReadInputRegs** — the *only* place this function
code appears anywhere in our captures.

### 6.1 The sweep

The sweep is six frames totalling 58 bytes on the wire, captured from
a setup where both CI60 (addr 2) and a second slave (addr 3) answer
FC04:

```
  off   bytes                                    decoded
  ───   ──────────────────────────────────────   ─────────────────────────────────
   +0   FE 04 00 00 00 04 E5 C6                  FC04 to 0xFE, regs 0..3  (no response)
   +8   01 04 00 00 00 04 F1 C9                  FC04 to 0x01, regs 0..3  (no response)
  +16   02 04 00 00 00 04 F1 FA                  FC04 to 0x02, regs 0..3
  +24   02 04 08 00 00 00 00 00 01 02 00 7B E9   ── CI60 responds (13 B)
        ^^addr ^^fc ^^bc payload (4 regs = 8 B) CRC
  +37   03 04 00 00 00 04 F0 2B                  FC04 to 0x03, regs 0..3
  +45   03 04 08 00 00 00 00 00 01 02 00 7F 15   ── second slave responds (13 B)
  +58   (sweep ends — CS60 moves on to bulk-init broadcasts §6.2)
```

CI60 answers `[0x0000, 0x0000, 0x0001, 0x0200]`. The fourth register
being `0x0200` looks suggestive of a firmware / protocol version
stamp (2.00?); the third being `0x0001` may be a device-type code
(CI60 = 1?), but neither is confirmed against documentation. Whether
CS60 accepts a distinct device-type value (e.g. `[0, 0, 2, 0x200]`)
is untested — in the capture above the second slave mirrors CI60's
payload exactly.

The 0xFE probe at the start is unexplained — no slave responds.
Modbus reserves `0xFE` only loosely (some implementations use it as a
"any-slave" or "controller-self" address); here it's most likely a
no-op CS60 prologue.

After the 0x03 probe the sweep terminates. Whether CS60 stops because
it polled a fixed range (0x01..0x03) or because it found at least
one responder and the first subsequent silence, is undetermined — the
captured sweep doesn't disambiguate.

### 6.2 Bulk register broadcast

Immediately after the sweep, CS60 broadcasts its full register state
to the bus via **four chained FC10 broadcasts**:

| Frame length | start | qty | covers regs        |
|-------------:|------:|----:|--------------------|
|  255 B       | 0x0000 | 123 | 0x0000..0x007A   |
|  255 B       | 0x007B | 123 | 0x007B..0x00F5   |
|  181 B       | 0x00F6 |  86 | 0x00F6..0x014B   |
|   49 B       | 0x014C |  20 | 0x014C..0x015F   |

Together they cover **regs 0x0000 through 0x015F** — exactly the
address range CS60 will subsequently poll (FC01 read-coil pairs at
`start=0x0000 qty=332` and `start=0x014C qty=352`). It functions as a
"sync to slaves" pulse: any newly-enrolled slave gets the full
current state pushed into its register/coil tables before steady-state
polling begins. ESPHome's `setHoldingRegister(addr,value)` /
`setCoil(addr,bool)` callbacks handle these the same way they handle
the steady-state FC10 status block.

### 6.3 Resumption of steady-state polling

The very next frame after the four bulk-init broadcasts is the
canonical FC10 status block at `0x00BE qty=85`, followed by FC01
pairs against every address that answered FC04.

---

## 7. CS60 behavioural observations

A few facts about CS60 behaviour that the captures establish but
that don't fit cleanly inside the per-function-code descriptions:

1. **Read-only observers don't need to register.** The FC10 status
   broadcast at `0x00BE` (85 regs, every ~120 ms) is broadcast to
   addr 0x00; any listener on the bus sees it. A passive sniffer
   that never answers FC04 captures MODE, all four temperature
   inputs, all four percentage outputs and the runtime counters in
   full, without ever joining the polled-slave set.

2. **Commands enter the system only via the slave coil + FC65
   cycle.** A device that wants to *write* (e.g. change MODE) has
   to be enrolled at boot, raise a coil, and let CS60 run the
   FC03 → FC65 follow-up. There is no master-side "write" path
   available to a non-slave.

3. **CS60 tolerates missed FC01 responses.** A registered slave
   that fails to respond to one or both FC01 reqs is not
   de-registered; CS60 simply polls it again on the next ~120 ms
   cycle. Slaves with imperfect response timing therefore keep
   their enrolment — any raised coil is still picked up within
   one or two cycles. (Confirmed by deliberately introducing a
   slave with a slower receive pipeline: 54 % FC01 hit rate, full
   command cycle still works because CS60's FC03 read happens
   with enough time tolerance.)

4. **Sensor-not-present sentinel.** Temperature inputs that have
   no probe wired report 16-bit signed sentinel values in the
   FC10 status block — `-187.5 °C`, `-250.0 °C`, etc. These are
   not real readings; downstream consumers must surface them as
   "sensor not present" rather than passing the raw value through.

### Open questions

- **FC04 payload semantics.** CI60's answer is
  `[0x0000, 0x0000, 0x0001, 0x0200]`. Whether `0x0001` encodes
  "device type = CI60", whether `0x0200` is a firmware/protocol
  version, and whether CS60 actually checks any of it (vs just
  accepting any valid CRC'd response) is untested. A capture with
  a second slave answering distinct values would disambiguate.
- **Enumeration sweep boundary.** The captured sweep hits
  `{0xFE, 0x01, 0x02, 0x03}` and stops. Whether CS60 always probes
  exactly that range, or stops at the first silence after a
  responder, is undetermined.
- **Unnamed status-block registers.** The 50 or so registers
  between `0x00CB` and `0x0103` carried by the FC10 status
  broadcast are mostly stable zeros plus a few constants (`0xDD75`,
  `0x001A`, etc.); none changed during the panel-press capture.
  Names and meanings unknown.
- **`REG_UNKNOWN_1` (`0x00C0`)** holds `0x0DA6` in the idle/panel
  captures and varies in the boot captures (`0x0DBB` etc.). Most
  likely an uptime counter on a slower clock; not decoded.

---

## 8. Cross-check against the ESPHome reference

The bus protocol observed here is broadly the dialect documented in
MSkjel's [esphome-flexit-modbus-server](https://github.com/MSkjel/esphome-flexit-modbus-server).
The major divergence worth flagging is around **FC04**:

> ESPHome disables FC04 via `MODBUS_DISABLE_*` macros — assuming the
> CS60 never issues it. The captures show otherwise: the CS60 uses
> FC04 to enrol slaves at boot (§6). A slave built around the
> ESPHome reference therefore relies on being polled by some other
> mechanism, or on the CS60 falling back to a broader sweep if its
> FC04 response is missing — neither is documented. With an explicit
> FC04 handler in the slave, enrolment is direct and reliable.

Otherwise the captures confirm the source: CS60 ignores Modbus-RTU
inter-frame timing, FC01 is the "big read" against the slave, FC03
is single-register reads only, FC65 is the proprietary reset that
acks slave-raised coils, coil X and holding reg X are paired, the
status block layout matches the documented register names where they
exist, and the four MODE values (Stop=0, Min=1, Normal=2, Max=3)
match the observed FC65 ack values.

Things the source doesn't cover and these captures don't resolve:
names for the registers between `0x00CB` and `0x0103`, the exact
semantics of the sensor-not-present sentinels, and the boot
enumeration sequence (§6) which has no analogue in the source.
