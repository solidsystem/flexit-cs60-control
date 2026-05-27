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

Two captures analyzed:

| File                                       | Size      | Duration | Byte rate | Notes |
|--------------------------------------------|-----------|----------|-----------|-------|
| `trafficdata/rs485-idle.bin`               |  41 770 B | ~18 s    | 2 320 B/s | Idle baseline — no panel interaction. |
| `trafficdata/rs485-panel-up-down.bin`      | 130 260 B | ~54 s    | 2 394 B/s | Speed setting changed via panel: min → medium → max → medium → min, a few seconds between presses. |

Both captures show the same byte rate, which corresponds to ~16–17 %
bus utilisation at 115200 baud. The bus is busy — the CS60 polls
continuously at ~8 cycles/s.

---

## 1. Bus parameters

| Parameter | Value | How verified |
|-----------|-------|-------------|
| Baud rate | **115200 / 8N1** | Frames decode cleanly. Matches the ESPHome Flexit-Modbus-Server reference. |
| Wiring    | Half-duplex RS485, DE/RE held low (receive-only) | We see master and slave traffic on the same line. |
| Bus master | Flexit **CS60** | All polling traffic originates from one party. |
| Polled slave | Flexit **CI60 panel @ addr 2** | All addr-2 traffic; no other unicast addresses ever appear. |
| Broadcasts | addr `0x00` | All FC06, FC10 and FC65 use addr 0 (Modbus broadcast). |

The CS60 is the only master. The CI60 panel only responds — never
initiates. Broadcasts are not echoed by slaves.

> The CS60 *intentionally ignores* Modbus-RTU inter-frame timing — see
> `setup()` in MSkjel's `flexit_modbus_server.cpp`:
> *"The CS/CU/CE60 doesnt actually follow the Modbus RTU spec. It just
> ignores any interframe timeout and blasts request."*
> This is why the receive path has to be DMA-driven: the master will
> happily send a second frame the byte after the first ends.

---

## 2. Polling cadence

```
~120 ms cycle, repeated continuously:
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

In the panel capture the CS60 issues **450 FC10 broadcasts in 54 s**
(8.3 broadcasts / s) and **1 626 FC01 requests** (30 req/s = 8.3
request *pairs* / s — matches). FC06 broadcasts run at ~3.7 / s. So
every fan-speed change carries through to slaves within 120 ms.

---

## 3. Function codes observed

Cross-referenced against MSkjel's ESPHome implementation, which
explicitly disables FC04 (ReadInputRegs), FC05 (WriteSingleCoil),
FC0F (WriteMultipleCoils) and FC11 (ReportSlaveID) via
`MODBUS_DISABLE_*` macros. With the improved receive path we now see
**every** code the CS60 actually uses.

A length-ascending CRC scan over both captures finds the following
function codes — and **only** these:

| FC | Idle capture | Panel capture |
|----|-------------:|--------------:|
| `0x01` ReadCoils | 514 | 1 626 |
| `0x03` ReadHoldingRegs | 0 | 6 |
| `0x06` WriteSingleReg | 67 | 203 |
| `0x10` WriteMultipleRegs | 146 | 450 |
| `0x65` Flexit reset | 0 | 3 |
| `0x00` (false-positive CRC coincidences) | 0 | 3 |

So **FC04, FC05, FC0F and FC11 are confirmed absent in both
captures** — the CS60 never issues them, matching what the ESPHome
implementation expects. FC02 (ReadDiscreteInputs) and FC17
(ReadWriteMultipleRegs) are also absent, even though ESPHome doesn't
explicitly disable those — the CS60 just doesn't use them.

The three `fc=0x00` hits in the panel capture are CRC coincidences
inside corrupted long frames (lengths 106, 159, 195 — not matching
any real Modbus frame shape). They appear because the length-ascending
scanner finds the first valid CRC at *any* length, including random
matches inside payload data. They are not real frames.

| FC | Name | Direction | Seen in | What it carries |
|----|------|-----------|---------|-----------------|
| `0x01` | ReadCoils | CS60 → slave (addr 2) | both | Polls the slave's "pending command" coil bitmap. Two requests per cycle cover coils 0–683. Responses are 332-coil (47 B) and 352-coil (49 B). The bitmap is **all zeros** during idle, and we see exactly **one bit set** (coil 0) when the panel raises a command. |
| `0x03` | ReadHoldingRegs | CS60 → slave (addr 2) | panel only | Single-register reads, only issued *as a follow-up* after a slave has raised a coil. The request is 8 B (`02 03 00 00 00 01 84 39` = read 1 reg at addr 0); the response is 7 B (`02 03 02 NN NN CC CC`). We see this exactly **3 times in the panel capture**, **0 times during idle**. |
| `0x06` | WriteSingleReg | CS60 → broadcast | both | Runtime counters (operating-minutes per mode + filter + rotor). 67 frames in 18 s idle, 203 in 54 s panel. |
| `0x10` | WriteMultipleRegs | CS60 → broadcast | both | Bulk status block: 85 registers at `0x00BE`, 179-byte frame. **This is the CS60's "here's everything I know" broadcast** — pushed every ~120 ms whether or not anything changed. |
| `0x65` | Flexit proprietary "reset coil+register" | CS60 → broadcast | panel only | 8-byte frame `00 65 AH AL VH VL CC CC`. ESPHome handles it as `setHoldingRegister(addr, value); setCoil(addr, 0);` — clears the coil that the slave set, while stamping the register with the broadcast value. We see this exactly **3 times in the panel capture**, **0 times during idle** — once per command cycle, always paired with an FC03 read. |

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
fan-speed mapping **Min=50 % / Normal=69 % / Max=100 %**. Both `MODE`
and `PCT_SUPPLY_FAN` track the speed setting — the latter is the
configured fan setpoint table that the CS60 looks up per mode.

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
this capture (no temperature change, no timer button, etc.).

---

## 6. Implications for the project goal

The XIAO is a **smarthouse bridge**: it sits on the Flexit panel bus
on one side and exposes the ventilation system to a home-automation
controller on the other side, via BLE or Zigbee (transport not yet
decided).

That splits the work cleanly:

- **Panel-bus side** — speak the proprietary Flexit Modbus dialect
  documented above. *Read* via passive sniffing of FC10 broadcasts;
  *write* via the slave + coil + FC65 cycle observed in §5.2.
- **Smarthouse side** — expose the mirrored state and a small set
  of commands over BLE or Zigbee. The interface design is out of
  scope for this document.

### Confirmed by direct observation

1. **Read-only monitoring needs no Modbus slave.** The FC10 status
   broadcast at `0x00BE` (85 regs, every ~120 ms) carries MODE, all
   four temperature inputs, all four percentage outputs and the
   runtime counters. Just decoding the broadcast stream is enough
   for everything the smarthouse needs to *display*.

2. **Writing back to the system needs a Modbus slave.** Commands
   from outside the panel only enter the system via the coil + FC65
   handshake (§5.2). To dim/boost the fan from the smarthouse, the
   XIAO must register as a Modbus slave that the CS60 polls.

3. **The CS60 only polls/acks an active slave.** It reacts to coil
   flags — if no slave raises a coil, FC03 and FC65 never appear.
   So a slave that never asserts a coil is effectively invisible to
   the CS60. For pure read-only operation, no slave is needed at
   all.

4. **The complete command cycle** for a slave-initiated command is:
   1. Slave sets `coil[X] = 1`, `holding_reg[X] = value`.
   2. CS60 sees the coil on its next FC01 ReadCoils poll (~120 ms).
   3. CS60 issues FC03 `read 1 reg at addr X`, slave responds with
      the value.
   4. CS60 broadcasts FC65 `(addr=X, value=value)`. The slave must
      treat this as `setHoldingRegister(X, value); setCoil(X, 0);`.
   5. CS60 broadcasts FC10 status block reflecting the new state.

5. **Slave addressing**: the panel sits at addr `2`. Any address ≠ 2
   (and ≠ 0 broadcast) is free. ESPHome defaults to `1`; `3` is
   another reasonable choice.

6. **Sensor-not-present sentinel**: extract-air and return-water
   sensors absent from this installation produce 16-bit signed values
   that decode as -187.5 °C, -250.0 °C, etc. These are not real
   readings; they're sentinel codes. The smarthouse side should
   surface them as "sensor not present" rather than passing the raw
   degree value through.

7. **Fan-speed table**: in this installation, Min=50 %, Normal=69 %,
   Max=100 %. These are the per-mode setpoints stored in `CMD_*`
   registers (`0x02/0x03/0x04` for supply fan, `0x07/0x08/0x09` for
   extract fan) — i.e. they're configurable on the panel side.

### Open / still to verify

- Whether the CS60 enumerates slaves only at boot, or also at
  runtime — neither capture covered a CS60 power cycle. ESPHome
  defaults to `setup_priority::BUS` to be ready early; we should do
  the same.
- The semantics of `REG_UNKNOWN_1` (`0xC0`) and `REG_UNKNOWN_2`
  (`0xC1`). `UNK_1` is `0x0DA6` (3494) in both captures and doesn't
  appear to change with mode — likely an uptime counter on a slower
  clock. `UNK_2` is consistently `0x0000`.
- The 50 or so unnamed registers between `0x00CB` and `0x00FF` that
  the FC10 broadcast also carries. Their values are mostly stable
  zeros plus a few constants (`0xDD75`, `0x001A`, etc.); none changed
  during the panel-press capture.

### Required next steps on the panel-bus side

1. **Phase 1 — passive monitoring.** Parse the FC10 broadcast into a
   local mirror of the panel state (MODE, four temperatures, four
   percentages, the runtime counters) and surface a few sentinel
   constants so the smarthouse layer can show "sensor n/a". This is
   strictly read-only — no slave registration, no risk of disturbing
   the existing CS60↔panel conversation.

2. **Phase 2 — Modbus slave for writes.** Register at addr `3`
   covering the five function codes the CS60 actually uses:
   - **FC01 ReadCoils** — keep a coil table of at least the
     `CMD_*` addresses we want to push (start with coil 0 = CMD_MODE).
   - **FC03 ReadHoldingRegs** — respond to single-register reads
     against any address we've armed via coil.
   - **FC06 / FC10** broadcasts from addr 0 — already handled by
     phase 1 (passive mirror); just keep doing it.
   - **FC65** broadcasts — clear the matching coil and stamp the
     matching register with the carried value (closes the cycle the
     slave opened).

   Per ESPHome, disable FC04, FC05, FC0F, FC11.

3. **Coil ↔ register address pairing.** Every command we want to
   issue needs storage in *both* tables at the same address. The
   coil is the "fresh" flag; the register holds the value.

4. **Pick command slots from the ESPHome `HoldingRegisterIndex`
   enum.** Useful targets: `0x00` (CMD_MODE), `0x02..0x09` (fan
   setpoints), `0x0C..0x0E` (temperature setpoints), `0x13A..0x13C`
   (timer / clear alarms), `0x116` (clear filter alarm). For a first
   write test, mirroring the panel's behaviour with `CMD_MODE` (coil
   0 + reg 0) is the cleanest starting point — the captured cycle in
   §5.2 is exactly what our slave needs to emit.

---

## 7. Cross-check against the ESPHome reference

Source files mirrored locally to `/tmp/esphome_ref/` from
`MSkjel/esphome-flexit-modbus-server` (`flexit_modbus_server.cpp` and
`flexit_modbus_server.h`, main branch).

### What the captures confirm in the ESPHome source

| Claim in ESPHome source | Confirmed by capture |
|-------------------------|---------------------|
| CS60 ignores Modbus RTU inter-frame timing | back-to-back frames, no idle gap |
| FC01 "big read" against the slave | exactly the two-request pair to addr 2 |
| FC03 = single-register reads only | every FC03 response is 7 B (1 reg) |
| FC65 = proprietary reset that ACKs slave commands | 3/3 captured presses end with an FC65 broadcast |
| Coil X and holding reg X are paired | panel raises coil 0; CS60 reads reg 0; FC65 carries (addr=0, value=N) |
| Function-code surface = {01, 03, 06, 10, 65}; FC04/05/0F/11 disabled | only these five appear in either capture; the four disabled codes are absent |
| Status block at `0x00BE`, MODE at `0x00BF`, sensors at `0x00C3..0x00C6` | FC10 broadcasts decode exactly that layout |
| 4 modes: Stop=0, Min=1, Normal=2, Max=3 | only 1/2/3 observed (panel never went to Stop in this run) — fan PCTs 50/69/100 % cross-check |

### Things the capture clarifies vs. the source

- The **status broadcast is much faster than the source suggests** —
  the ESPHome implementation processes whatever the CS60 sends; it
  doesn't document a cadence. We measured ~8 broadcasts/s = one every
  120 ms.
- The CS60's `start=0x0200` field in some "FC03 response" frames is
  a red herring — those are CRC coincidences inside other frame
  payloads, not real FC03 requests. Real FC03 requests use
  `start=0x0000 count=1`.

### Things still not in the source

- Names for the registers between `0x00CB` and `0x0103`. The ESPHome
  enum jumps from `0x00CA` (PCT_SUPPLY_FAN) straight to `0x0104`
  (ALARM_SENSOR_SUPPLY_FAULTY).
- The exact semantics of the sensor-not-present sentinel values.
  ESPHome treats raw values as `int16_t / 10`, so it would just show
  -125 °C in the UI; the CS60 must have a separate flag we haven't
  located.

---

## 8. Tooling used

- `tools/ble-client stream <file>` — streams live RS485 bytes to
  disk over BLE NUS.
- `/tmp/analyze_modbus.py` — short-first CRC scan, frame stats,
  register decode using ESPHome's name table.
- `/tmp/fast_analyze.py` — length-prioritised CRC scan with table-
  driven CRC for faster analysis of large captures; extracts a
  chronological MODE timeline from FC10 broadcasts.
- `/tmp/show_cycle.py` — locates the four "interesting" event
  classes (FC01 non-zero bitmap, FC03 req+resp, FC10 MODE change,
  FC65 broadcast) and prints them with timestamps so the full
  per-press cycle is one screen.

All Python helpers re-implement the Modbus CRC-16 (poly 0xA001, init
0xFFFF), so they're standalone — no external dependencies.

---

## 9. Receive-path notes (for posterity)

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
