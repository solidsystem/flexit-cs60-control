---
name: flash-monitor
description: Build the flexitMC3 firmware, flash it over BLE DFU, and stream the BLE NUS console — all without any USB cable to the XIAO. Use whenever a code change needs to be tried on real hardware, or whenever you need to read runtime printk/log output (RS485 listener traces, BLE pairing events, etc.) to diagnose behaviour.
---

# Build, flash, and monitor flexitMC3 over Bluetooth

The board advertises as `flexitMC3` and exposes two BLE services we drive from
macOS (CoreBluetooth, no HCI dongle):

- **SMP / MCUmgr** — receives signed-image uploads and reset commands (DFU).
- **Nordic UART Service (NUS)** — streams `printk` / log output as notifications.

Only one BLE connection is accepted at a time (`CONFIG_BT_MAX_CONN=1`). The
console client must be stopped before starting a flash/confirm. All three
Python tools use bleak's `async with BleakClient(...)` context manager, so a
normal exit / Ctrl-C / SIGTERM triggers a clean disconnect; the device then
resumes advertising and the next client can connect without a manual reset.

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

## Monitor the NUS console

### Foreground (interactive — only when the user is driving)

```bash
python3 tools/ble_console.py
```

### Background (the pattern I use to capture output between steps)

Always launch with `python3 -u` so prints flush immediately, redirect to a
known logfile under `run/`, and stash the PID so we can stop it cleanly:

```bash
mkdir -p run
python3 -u tools/ble_console.py > run/console.log 2>&1 &
echo $! > run/console.pid
```

Read what has been captured so far:

```bash
cat run/console.log
```

Stop the capture (mandatory before any flash/confirm — there's only one BLE
connection slot and `ble_console.py` holds it while running):

```bash
[ -f run/console.pid ] && kill "$(cat run/console.pid)" 2>/dev/null
rm -f run/console.pid
```

When invoking from the Bash tool, prefer launching the background capture
with `run_in_background: true` instead of `&` — that way the Claude harness
manages the lifecycle and I'll be notified if the script crashes. Still
redirect output to `run/console.log` so I can read it via `cat`.

The `run/` directory is gitignored (entry added alongside `build*/`). It
holds anything ephemeral I generate while driving the device — logs, PID
files, captured frames, etc. Safe to wipe between sessions.

## Full dev cycle (one block, copy-paste safe)

```bash
# 0. Stop any previous console capture so we don't lock the BLE slot.
[ -f run/console.pid ] && kill "$(cat run/console.pid)" 2>/dev/null
rm -f run/console.pid
mkdir -p run

# 1. Build.
tools/build.sh

# 2. Flash over BLE.
python3 tools/ble_dfu_flash.py

# 3. Give the device a few seconds to reboot + start advertising again.
sleep 8

# 4. Start console capture in the background.
python3 -u tools/ble_console.py > run/console.log 2>&1 &
echo $! > run/console.pid

# 5. Wait for output, then inspect.
sleep 5
cat run/console.log
```

## Passkey caveat (read this if a step hangs)

The device has `CONFIG_BT_BONDABLE=n` — pairing state isn't persisted across
reboots. macOS may pop up a system dialog asking for the 6-digit passkey
`444999` (the value of `FIXED_PASSKEY` in
[src/ble_transport.c](../../../src/ble_transport.c)). I cannot respond to
that dialog programmatically — if either Python script stalls right after
printing `Connected.`, ask the user to enter `444999` in the macOS prompt.

If pairing fails repeatedly, the user can clear macOS's stale entry under
**System Settings → Bluetooth → (i) next to `flexitMC3` → Forget**, then
retry.

## Reading and reporting console output

Console lines that matter for current debugging:

- `RS485 RX (n): XX XX ...` — frame-grouped UART listener (see
  [src/main.c](../../../src/main.c) `flush_work_handler`).
- `BLE connected: …` / `BLE disconnected: …` / `BLE security raised …` —
  pairing and link status.
- `bt_le_adv_start failed: -N` — unexpected advertising error.

When summarising captured output back to the user, prefer pasting the
relevant lines verbatim — line counts and exact byte sequences matter when
diagnosing RS485 framing issues.
