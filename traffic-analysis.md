# RS485 traffic analysis — Flexit CI60 / CS60 ventilation system

Captures taken with the on-board RS485 listener on the XIAO BLE
(see `src/rs485_uart.c`, `src/ble_transport.c`), streamed live to file
via `tools/ble-client stream`.

The receive path uses the Zephyr async UART API on the UARTE peripheral
with **double-buffered DMA** (2 × 256 B + TIMER2 for byte counting).
The earlier interrupt-driven path dropped bytes whenever BLE host work
held off the per-byte ISR past the UARTE's 16-byte FIFO; every long
frame on the bus was therefore corrupted in flight. With DMA, long
frames now arrive intact and **96–97 %** of captured bytes form valid
Modbus CRC frames.

Three captures analyzed:

| File                                       | Size      | Duration | Byte rate | Notes |
|--------------------------------------------|-----------|----------|-----------|-------|
| `trafficdata/rs485-idle.bin`               |  41 770 B | ~18 s    | 2 320 B/s | Idle baseline — no panel interaction. CI60 only. |
| `trafficdata/rs485-panel-up-down.bin`      | 130 260 B | ~54 s    | 2 394 B/s | Speed setting changed via panel: min → medium → max → medium → min, a few seconds between presses. CI60 only. |
| `trafficdata/rs485-init.bin`               | varies    | varies   | —         | CS60 cold-boot: XIAO already running and listening on the bus, then CS60 (+ CI60) powered on while streaming. Contains the FC04 enumeration sweep, §6. |

The idle and panel captures show ~16–17 % bus utilisation at 115200
baud during steady-state polling (~8 cycles/s). The init capture covers
the much rarer boot transient (§6).

---

## 1. Bus parameters

| Parameter | Value | How verified |
|-----------|-------|-------------|
| Baud rate | **115200 / 8N1** | Frames decode cleanly. Matches the ESPHome Flexit-Modbus-Server reference. |
| Wiring    | Half-duplex RS485, DE/RE held low (receive-only on the sniffer; the slave side now also drives TX) | We see master and slave traffic on the same line. |
| Bus master | Flexit **CS60** | All polling traffic originates from one party. |
| Polled slaves | Set chosen at boot via FC04 enumeration (§6). Currently observed: **CI60 panel @ addr 2** + **XIAO @ addr 3** | Both addresses receive FC01 polls every cycle. |
| Broadcasts | addr `0x00` | All FC06, FC10 and FC65 use addr 0 (Modbus broadcast). |

The CS60 is the only master. Slaves only respond — never initiate
unsolicited traffic. Broadcasts are not echoed by slaves.

> **Slave set is fixed at boot.** The CS60 sweeps addresses 0x01..0x03
> (plus 0xFE) with FC04 ReadInputRegs once at power-up; whoever
> answers becomes a polled slave for the rest of the session. Flipping
> the CI60 panel's DIP-switch from off to on moves it from addr 2 to
> addr 3, but only takes effect after the next CS60 power-cycle.
> Adding a second slave (e.g. XIAO at 3 while CI60 stays at 2) also
> requires a CS60 power-cycle to enrol — runtime additions are
> invisible. See §6 for the full enumeration sequence.

> The CS60 *intentionally ignores* Modbus-RTU inter-frame timing — see
> `setup()` in MSkjel's `flexit_modbus_server.cpp`:
> *"The CS/CU/CE60 doesnt actually follow the Modbus RTU spec. It just
> ignores any interframe timeout and blasts request."*
> This is why the receive path has to be DMA-driven: the master will
> happily send a second frame the byte after the first ends.

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

When two slaves are registered (e.g. CI60 + XIAO), the CS60 fires the
FC01 pair at each registered address back-to-back with no idle gap
between them — so each slave gets the same ~8 polls / s, total bus
load roughly doubles for the FC01 traffic. The FC10 broadcast cadence
does **not** double; it stays at ~8 / s regardless of slave count.

---

## 3. Function codes observed

Cross-referenced against MSkjel's ESPHome implementation, which
explicitly disables FC04 (ReadInputRegs), FC05 (WriteSingleCoil),
FC0F (WriteMultipleCoils) and FC11 (ReportSlaveID) via
`MODBUS_DISABLE_*` macros. With the improved receive path we now see
**every** code the CS60 actually uses — including one (FC04) that
ESPHome disables but that turns out to be required for slave
registration; see §6.

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

So **FC05, FC0F and FC11 are confirmed absent** during steady-state —
the CS60 never issues them. FC04 is also absent from steady-state but
is the workhorse of the boot-time enumeration (§6). FC02
(ReadDiscreteInputs) and FC17 (ReadWriteMultipleRegs) are absent too,
even though ESPHome doesn't explicitly disable those — the CS60 just
doesn't use them.

The three `fc=0x00` hits in the panel capture are CRC coincidences
inside corrupted long frames (lengths 106, 159, 195 — not matching
any real Modbus frame shape).

| FC | Name | Direction | Seen in | What it carries |
|----|------|-----------|---------|-----------------|
| `0x01` | ReadCoils | CS60 → slave | all | Polls the slave's "pending command" coil bitmap. Two requests per cycle cover coils 0–683. Responses are 332-coil (47 B) and 352-coil (49 B). The bitmap is **all zeros** until a slave raises a command flag (coil 0 = CMD_MODE in the captured cases). |
| `0x03` | ReadHoldingRegs | CS60 → slave | panel + init | Single-register reads, only issued *as a follow-up* after a slave has raised a coil. The request is 8 B (`02 03 00 00 00 01 84 39` = read 1 reg at addr 0); the response is 7 B (`02 03 02 NN NN CC CC`). |
| `0x04` | ReadInputRegs | CS60 → slave (probe) | **init only** | Boot-time enumeration sweep — 8 B request asking for input regs 0..3. Used to discover which addresses host a slave. See §6. |
| `0x06` | WriteSingleReg | CS60 → broadcast | all | Runtime counters (operating-minutes per mode + filter + rotor). 67 frames in 18 s idle, 203 in 54 s panel. |
| `0x10` | WriteMultipleRegs | CS60 → broadcast | all | Two distinct shapes: (a) the **status block** at `0x00BE` (85 regs, 179 B, every ~120 ms), and (b) a **bulk-init burst** that appears only at boot — four chained writes covering regs `0x0000..0x015F`, see §6. |
| `0x65` | Flexit proprietary "reset coil+register" | CS60 → broadcast | panel + init | 8-byte frame `00 65 AH AL VH VL CC CC`. ESPHome handles it as `setHoldingRegister(addr, value); setCoil(addr, 0);` — clears the coil that the slave set, while stamping the register with the broadcast value. Once per command cycle, always paired with an FC03 read. |

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

Panel coil bitmap is **all zeros** throughout: no slave is raising any
command flag while idle. Consequently the CS60 issues no FC03 reads
and no FC65 resets.

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

## 6. Boot enumeration (`rs485-init.bin`)

The init capture was taken with the XIAO already streaming and the
CS60 + CI60 powered off, then powered on. The first thing CS60 emits
after boot is a single enumeration sweep using **FC04 ReadInputRegs**
— the *only* place this function code appears anywhere in our
captures.

### 6.1 The sweep

The sweep is six frames totalling 58 bytes on the wire (captured
from `trafficdata/rs485-init-3.bin` with XIAO answering FC04 and
firmware-side TX echo enabled — see §10):

```
  off   bytes                                    decoded
  ───   ──────────────────────────────────────   ─────────────────────────────────
   +0   FE 04 00 00 00 04 E5 C6                  FC04 to 0xFE, regs 0..3  (no response)
   +8   01 04 00 00 00 04 F1 C9                  FC04 to 0x01, regs 0..3  (no response)
  +16   02 04 00 00 00 04 F1 FA                  FC04 to 0x02, regs 0..3
  +24   02 04 08 00 00 00 00 00 01 02 00 7B E9   ── CI60 responds (13 B)
        ^^addr ^^fc ^^bc payload (4 regs = 8 B) CRC
  +37   03 04 00 00 00 04 F0 2B                  FC04 to 0x03, regs 0..3
  +45   03 04 08 00 00 00 00 00 01 02 00 7F 15   ── XIAO responds (13 B)
  +58   (sweep ends — CS60 moves on to bulk-init broadcasts §6.2)
```

Both CI60 and XIAO answer `[0x0000, 0x0000, 0x0001, 0x0200]` — XIAO
mirrors CI60's payload exactly (only the CRC differs because of the
addr byte). The fourth register being `0x0200` looks suggestive of a
firmware / protocol version stamp (2.00?); the third being `0x0001`
may be a device-type code (CI60 = 1?), but neither is confirmed
against documentation. Whether CS60 accepts a distinct device-type
value (e.g. `[0, 0, 2, 0x200]`) is untested.

The 0xFE probe at the start is unexplained — no slave responds.
Modbus reserves `0xFE` only loosely (some implementations use it as a
"any-slave" or "controller-self" address); here it's most likely a
no-op CS60 prologue.

After the 0x03 probe, the sweep terminates. Whether CS60 stops because
it polled a fixed range (0x01..0x03) or because it found at least one
responder and the first subsequent silence, is undetermined — the
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
pairs against every address that answered FC04. With CI60 at 2 and
XIAO at 3 (XIAO answering FC04 since the firmware change of
`src/flexit_slave.c`), both slot 2 *and* slot 3 receive the full
FC01 pair every cycle.

### 6.4 Implications

- **Slave enrolment is FC04-gated**. A slave that doesn't answer the
  FC04 probe at boot will be silently ignored for the rest of the
  CS60 session — no FC01 polls, no FC03 reads, no FC65 acks.
  XIAO therefore needs an FC04 handler at minimum 4 input registers
  (regs 0..3); see `flexit_slave.c::build_fc04_response`.
- **Enrolment only happens at CS60 boot**. Plugging in a new slave
  at runtime is invisible — the CS60 has to be power-cycled (with
  the XIAO already running and reachable on the bus) for the new
  slave to be picked up.
- **Whether the FC04 response payload is checked is unknown**. XIAO
  currently mirrors CI60's `[0, 0, 1, 0x200]` exactly and is accepted.
  Whether a distinct device-type would also be accepted, or whether
  CS60 demands `[0, 0, 1, 0x200]` specifically, is untested.

---

## 7. Implications for the project goal

The XIAO is a **smarthouse bridge**: it sits on the Flexit panel bus
on one side and exposes the ventilation system to a home-automation
controller on the other side, via BLE or Zigbee (transport choice
still open; current implementation uses BLE NUS).

That splits the work cleanly:

- **Panel-bus side** — speak the proprietary Flexit Modbus dialect
  documented above. *Read* via passive sniffing of FC10 broadcasts;
  *write* via the slave + coil + FC65 cycle observed in §5.2; *get
  registered* via the FC04 handshake in §6.
- **Smarthouse side** — expose the mirrored state and a small set
  of commands over BLE. See the `BLE client` section of `CLAUDE.md`.

### Confirmed by direct observation

1. **Read-only monitoring needs no Modbus slave.** The FC10 status
   broadcast at `0x00BE` (85 regs, every ~120 ms) carries MODE, all
   four temperature inputs, all four percentage outputs and the
   runtime counters. Decoding the broadcast stream is enough for
   everything the smarthouse needs to *display*.

2. **Writing back to the system needs a registered Modbus slave.**
   Commands from outside the panel only enter the system via the
   coil + FC65 handshake (§5.2). The CS60 will only ever poll a slave
   it enrolled at boot (§6), so the slave must answer FC04 *and* be
   present on the bus when CS60 powers up.

3. **The CS60 reacts to coil flags.** If no slave raises a coil, FC03
   and FC65 never appear. A registered slave that never asserts a
   coil is benign — it just gets polled with FC01 and answers
   all-zero bitmaps.

4. **The complete command cycle** for a slave-initiated command is:
   1. Slave sets `coil[X] = 1`, `holding_reg[X] = value`.
   2. CS60 sees the coil on its next FC01 ReadCoils poll (~120 ms).
   3. CS60 issues FC03 `read 1 reg at addr X`, slave responds with
      the value.
   4. CS60 broadcasts FC65 `(addr=X, value=value)`. The slave treats
      this as `setHoldingRegister(X, value); setCoil(X, 0);`.
   5. CS60 broadcasts FC10 status block reflecting the new state.

5. **Slave addressing**: addr `2` is the CI60 panel by default.
   XIAO sits at addr `3` (`FLEXIT_SLAVE_ADDR` in `flexit_slave.h`).
   Both are simultaneously polled in the current setup. Addr `1` is
   probed by the boot sweep but currently unoccupied.

6. **Sensor-not-present sentinel**: extract-air and return-water
   sensors absent from this installation produce 16-bit signed values
   that decode as -187.5 °C, -250.0 °C, etc. These are not real
   readings; they're sentinel codes. The smarthouse side surfaces
   them as "sensor not present" (see `panel_mirror.c`).

7. **Fan-speed table**: in this installation, Min=50 %, Normal=69 %,
   Max=100 %. These are the per-mode setpoints stored in `CMD_*`
   registers (`0x02/0x03/0x04` for supply fan, `0x07/0x08/0x09` for
   extract fan) — configurable on the panel side.

### Status of the implementation

- **Phase 1 — passive monitoring**: done. `src/panel_mirror.c`
  decodes the FC10 status block live and exposes the state over BLE
  (`ble-client state`).
- **Phase 2 — Modbus slave for writes**: done.
  `src/flexit_slave.c` implements FC01, FC03, FC04 (boot), and FC65;
  XIAO is registered at addr 3 after a CS60 power-cycle and the
  full command cycle (`ble-client mode N`) works end-to-end.

### Known limitations / open questions

- **FC01 response timing on XIAO** is degraded by the DMA RX
  pipeline. CS60 fires the two FC01 reqs (`start=0x0000 qty=332`
  and `start=0x014C qty=352`) back-to-back with no idle gap. XIAO's
  DMA RX buffer is only released by either the 1 ms idle timeout
  (`RX_TIMEOUT_US` in `rs485_uart.c`) — which never fires inside a
  CS60 burst — or by buffer fill (256 B ≈ 22 ms in). XIAO therefore
  can't respond in the first poll's response slot consistently.
  Measured in the boot capture with TX echo enabled:

  | poll                          | hit rate |
  |-------------------------------|---------:|
  | first poll  (qty=332, coils 0..331)   | **32 %** |
  | second poll (qty=352, coils 332..683) | **84 %** |
  | combined                              | **54 %** |

  CI60 over the same window hits **97 %** on both polls. Every XIAO
  response that lands does so in the immediate `gap=0 B` slot — no
  late or misaligned responses. The CS60 does not react to a missed
  response; it just re-polls on the next cycle, so any raised coil
  is picked up within 1–2 cycles. The mode-change cycle is therefore
  unaffected: CS60's FC03 reg-0 read happens with enough time
  tolerance for XIAO to respond cleanly, and the FC65 ack is a
  broadcast that needs no response. Lowering `RX_TIMEOUT_US` to
  ~150 µs and reordering the drain pipeline to feed `flexit_slave`
  before BLE forwarding would improve the FC01 hit rate, but there
  is no functional reason to do so at present.
- **FC04 payload semantics** — see §6.4. XIAO mirrors CI60's exact
  4-register answer. Whether CS60 cares about the contents is
  untested.
- **Sweep boundary** — see §6.1. Whether CS60 always probes
  `{0xFE, 0x01, 0x02, 0x03}` or sweeps further is undetermined.
- **The 50 or so unnamed registers** between `0x00CB` and `0x0103`
  that the FC10 status block carries. Their values are mostly
  stable zeros plus a few constants (`0xDD75`, `0x001A`, etc.);
  none changed during the panel-press capture.
- **`REG_UNKNOWN_1` (`0xC0`)** is `0x0DA6` (3494) in the idle/panel
  captures, varies in init (`0x0DBB` and similar). Likely an uptime
  / wall-clock counter; not yet decoded. `UNK_2` is consistently
  `0x0000`.

---

## 8. Cross-check against the ESPHome reference

Source files mirrored locally to `/tmp/esphome_ref/` from
`MSkjel/esphome-flexit-modbus-server` (`flexit_modbus_server.cpp` and
`flexit_modbus_server.h`, main branch).

### What the captures confirm in the ESPHome source

| Claim in ESPHome source | Confirmed by capture |
|-------------------------|---------------------|
| CS60 ignores Modbus RTU inter-frame timing | back-to-back frames, no idle gap |
| FC01 "big read" against the slave | exactly the two-request pair to each registered addr |
| FC03 = single-register reads only | every FC03 response is 7 B (1 reg) |
| FC65 = proprietary reset that ACKs slave commands | 3/3 captured presses end with an FC65 broadcast |
| Coil X and holding reg X are paired | panel raises coil 0; CS60 reads reg 0; FC65 carries (addr=0, value=N) |
| Status block at `0x00BE`, MODE at `0x00BF`, sensors at `0x00C3..0x00C6` | FC10 broadcasts decode exactly that layout |
| 4 modes: Stop=0, Min=1, Normal=2, Max=3 | only 1/2/3 observed in user-driven presses — fan PCTs 50/69/100 % cross-check |

### Where the captures diverge from the source

- **ESPHome disables FC04**, but the CS60 *requires* FC04 to enrol a
  slave at boot (§6). A pure-ESPHome firmware would never get
  registered if it weren't already at an address the CS60 happens to
  remember from a previous boot — but since CS60 also has no NV slot
  memory (verified by moving CI60's DIP switch and rebooting CS60),
  this is not actually possible. The ESPHome reference therefore
  only works if the CS60 sweep range happens to include the slave's
  address *and* the slave somehow responds to it. With our firmware
  implementing FC04 explicitly, this dependency is now visible and
  reliable.
- The **status broadcast cadence is much faster than the source
  suggests** — the ESPHome implementation processes whatever the CS60
  sends; it doesn't document a cadence. We measured ~8 broadcasts/s
  = one every 120 ms.

### Things still not in the source

- Names for the registers between `0x00CB` and `0x0103`. The ESPHome
  enum jumps from `0x00CA` (PCT_SUPPLY_FAN) straight to `0x0104`
  (ALARM_SENSOR_SUPPLY_FAULTY).
- The exact semantics of the sensor-not-present sentinel values.
  ESPHome treats raw values as `int16_t / 10`, so it would just show
  -125 °C in the UI; the CS60 must have a separate flag we haven't
  located.
- The boot enumeration sequence (§6) — the source has no slave-
  registration logic at all.

---

## 9. Tooling used

- `tools/ble-client stream <file>` — streams live RS485 bytes to
  disk over BLE NUS.
- `tools/ble-client state [--human-friendly]` — reports decoded
  panel state and slave counters (FC01/FC03/FC04/FC65) over BLE.
- Throwaway Python helpers in `/tmp/` for offline frame analysis
  (length-prioritised CRC scan, frame stats, MODE timeline,
  enumeration window inspection). All implement Modbus CRC-16 (poly
  0xA001, init 0xFFFF) directly — no external dependencies.

---

## 10. Receive-path notes (for posterity)

The first attempts at this analysis used the Zephyr **interrupt-
driven** UART API. That couldn't keep up: long frames (FC10
broadcasts at 179 B, FC01 responses at 46–48 B) **all** failed CRC
because the per-byte ISR was sometimes blocked by BLE host work for
longer than the UARTE's 16-byte FIFO could buffer (~1.4 ms). Short
frames (8 B) survived; everything longer did not.

The fix was to move `uart0` to the **async UART API with double-
buffered DMA**:

- `prj.conf`: `CONFIG_UART_ASYNC_API=y`, plus
  `CONFIG_UART_0_INTERRUPT_DRIVEN=n` to override the per-instance
  default that MCUmgr's UART transport pulls in globally.
- `boards/xiao_ble_nrf52840.overlay`: enable `timer2` and reference
  it from `uart0` (`timer = <&timer2>;`) so the UARTE driver has a
  dedicated hardware byte counter — required for reliable async RX
  on nRF52 silicon when there's no hardware flow control.
- `src/rs485_uart.c`: two 256-byte DMA buffers, 1 ms idle timeout,
  bytes ferried via the existing ring buffer + drain worker.

Result: capture rate jumped from ~272 B/s (heavy loss) to ~2 400 B/s
(matches actual bus volume) and frame coverage from 33.7 % to 96.5 %.

### TX echo in captures

The SP3485 has RE# tied to DE, so while XIAO drives the line during
its own transmission the receiver is electrically disabled — bytes
XIAO puts on the wire are not echoed back into its DMA buffers.
Without intervention, any capture taken via `ble-client stream` /
`fetch` would only reflect what XIAO *received*, never what it
*transmitted*, leaving its own FC01/FC03/FC04/FC65 responses
invisible.

`rs485_uart_send()` therefore feeds the just-transmitted frame back
into `rs485_store_append()` and `ble_transport_forward_rs485()` on
successful TX. Captures from this point on contain the full bus
view — both directions — and let us measure response timing
directly (see §7 known limitations for the resulting FC01 hit-rate
analysis).
