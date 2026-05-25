---
name: flash-monitor
description: Build the flexitMC3 firmware, flash it over BLE DFU, and read the BLE NUS console dump — all without any USB cable to the XIAO. Use whenever a code change needs to be tried on real hardware, or whenever you need to read the stored RS485 traffic to diagnose behaviour.
---

# Build, flash, and monitor flexitMC3 over Bluetooth

The board advertises as `flexitMC3` and exposes two BLE services we drive from
macOS (CoreBluetooth, no HCI dongle):

- **SMP / MCUmgr** — receives signed-image uploads and reset commands (DFU).
- **Nordic UART Service (NUS)** — one-shot RS485 ring-buffer dump on connect.

Only one BLE connection is accepted at a time (`CONFIG_BT_MAX_CONN=1`).

## NUS console: dump-on-connect behaviour

The device does **not** stream logs continuously. Instead:

1. `ble_console.py` connects and subscribes to NUS TX notifications.
2. The device's `nus_send_enabled` callback fires, wakes the `rs485_dump`
   thread, which snapshots the RS485 ring buffer (up to 2 KB) and sends it
   over NUS as formatted hex lines.
3. After the dump the device **disconnects itself** (HCI reason 0x16 —
   Remote User Terminated Connection). `ble_console.py` prints `Disconnected.`
   and exits normally.

Output format:

```
RS485 RX (55): 01 03 00 2E 00 0A ...
RS485 RX (57): 01 03 14 00 01 ...
Disconnected.
```

If the ring buffer is empty:

```
(no RS485 frames stored)
Disconnected.
```

Because the device disconnects by itself, there is **no need to kill
`ble_console.py` before starting a flash or confirm** — the BLE slot is
already free.

## Build

Use [tools/build.sh](../../../tools/build.sh) — it sets the NCS toolchain env
vars documented in CLAUDE.md and runs the standard sysbuild invocation. Extra
flags are forwarded to `west build`.

Incremental:

```bash
tools/build.sh
```

Pristine (after devicetree / Kconfig structural changes):

```bash
tools/build.sh --pristine
```

Artifact consumed by the BLE flasher: `build/flexitMC3/zephyr/zephyr.signed.bin`.

The Python tools (`ble_dfu_flash.py`, `ble_console.py`) do **not** need the
NCS toolchain env — they only need `python3` and the vendored bleak / cbor2
under `tools/lib/`.

## Flash over BLE DFU

```bash
python3 tools/ble_dfu_flash.py
```

The script scans for `flexitMC3`, opens an SMP connection, uploads the signed
image (~30–60 s for ~150 kB at default chunk size), marks slot 1 for **test
boot**, then resets. MCUboot swaps on reboot. The test image is reverted on
the next reset unless we confirm it:

```bash
python3 tools/ble_dfu_flash.py --confirm
```

Only confirm once the new firmware has actually behaved correctly.

If the upload errors with `IMAGE_SETTING_TEST_TO_ACTIVE_DENIED` (rc=33), the
built image is bit-identical to what's already running — make any source
change and rebuild before retrying.

## Read the NUS dump

### Foreground (interactive or from Bash tool)

```bash
python3 tools/ble_console.py
```

The script connects, receives the dump, prints it, and exits when the device
disconnects. No kill step needed afterward.

### Capturing output to a file

```bash
mkdir -p run
python3 tools/ble_console.py > run/console.log 2>&1
cat run/console.log
```

Because the dump is self-terminating, no background PID management is needed.

## Full dev cycle (one block, copy-paste safe)

```bash
mkdir -p run

# 1. Build.
tools/build.sh

# 2. Flash over BLE.
python3 tools/ble_dfu_flash.py

# 3. Give the device a few seconds to reboot + start advertising again.
sleep 8

# 4. Read the RS485 dump (device auto-disconnects when done).
python3 tools/ble_console.py > run/console.log 2>&1
cat run/console.log
```

## Passkey and bonding

The device uses `CONFIG_BT_BONDABLE=y` with NVS-backed settings persistence.
After the **first** pairing (fixed passkey `444999` from `FIXED_PASSKEY` in
[src/ble_transport.c](../../../src/ble_transport.c)), both macOS and the device
store the LTK. Subsequent connections re-use the cached LTK without prompting.

The bond survives DFU flashes because `storage_partition` lives at `0xfe000`
outside MCUboot's swap slots.

If a passkey prompt appears (first pairing after bond wipe), enter `444999`.
If pairing fails repeatedly, clear macOS's stale entry under
**System Settings → Bluetooth → (i) next to `flexitMC3` → Forget**, then retry.

## Reading and reporting dump output

- `RS485 RX (n): XX XX ...` — one line per stored frame; `n` is the byte count.
- `(no RS485 frames stored)` — ring buffer was empty at connect time.
- `BLE connected: …` / `BLE disconnected: …` / `BLE security raised …` —
  visible in USB serial log, not in the NUS dump.

When summarising captured output back to the user, paste the relevant lines
verbatim — line counts and exact byte sequences matter when diagnosing RS485
framing issues.

The `run/` directory is gitignored. It holds anything ephemeral generated
while driving the device — logs, captured frames, etc. Safe to wipe between
sessions.
