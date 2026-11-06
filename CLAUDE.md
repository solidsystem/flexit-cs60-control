# flexit-cs60-control

## Project goal

The project goal is to create a smarthouse device that allows monitoring and controlling 
the flexit ventilation system. The smarthouse controller will connect to xiao_ble with
either bluetooth or zigbee (not decided which to implement yet).


## hardware info

**Device names**: 
 - CU60, CE60 and CS60 are all names for the flexit ventilation control unit.
 - CI60 is the name of the connected control panel 
 - CI600 is the name of another control panel model (not connected here)
 - CI66 is the name of a modbus adapter (not connected here)
 - XIAO is the smarthouse device in development

Zephyr/nRF Connect SDK application for Seeed XIAO BLE (nRF52840) with MCUboot
(https://wiki.seeedstudio.com/XIAO_BLE/).

The board sits on a **Seeed XIAO-RS485-Expansion-Board**
(https://wiki.seeedstudio.com/XIAO-RS485-Expansion-Board/). 

The RS485 transceiver is connected (via rj12 connector) to **flexit ci60** control panel, 
which is connected to **flexit cs60**. 

## Toolchain

- NCS version: v3.4.0
- SDK: `~/ncs/v3.4.0`
- Board: `xiao_ble/nrf52840`

When creating helper scripts/software, do it in golang if possible.

## Environment

Commands must be run with the NCS toolchain. The pattern is:

```bash
source ~/ncs/v3.4.0/zephyr/zephyr-env.sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- <command>
```

The firmware build requires the BLE pairing passkey in the environment — it is
baked into the image at build time and the build fails (CMake `FATAL_ERROR`) if
it is unset. Export it (a 6-digit number) before building; the toolchain wrapper
inherits the environment:

```bash
export FLEXIT_CS60_CONTROL_BLE_KEY=444999
```

**444999 is only an example.** The deployed device was built with a different,
secret passkey that is deliberately not in the repo. Build with the *same* key
the device already uses (ask the user if it is not in the environment) — an
image built with another key breaks fresh pairing after it is flashed (existing
bonds still work, which hides the mistake). Building with a new key changes the
passkey for every future pairing.

## Common Commands

```bash
# Build
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --build-dir build

# Pristine build (clean slate)
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --pristine --build-dir build

# Kconfig menu
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
    west build --build-dir build -t menuconfig

# Clean
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
    west build --build-dir build -t clean
```

## Config / DFU pitfalls that can brick the device

These two mistakes together bricked the device once (hung pre-USB with no
auto-revert; recovery required SWD re-flash via the nRF52840DK — see "Recovering
a bricked board" below). Avoid them:

- **Do NOT enable `CONFIG_LOG_MODE_IMMEDIATE`.** The console is the USB CDC-ACM
  port (`zephyr,console = board_cdc_acm_uart`). Immediate logging writes
  synchronously to that port, and during early boot — before USB enumerates and
  while no host is draining it — the first log call **blocks**, hanging the boot
  so USB never comes up. Keep `CONFIG_LOG_MODE_DEFERRED=y`. To capture a crash
  that deferred logging loses on reboot, prefer halting over rebooting (e.g.
  `CONFIG_ZBOSS_HALT_ON_ASSERT=y`, or disable `CONFIG_RESET_ON_FATAL_ERROR` in a
  debug build) so the message flushes and the port stays up — not immediate mode.

- **Do NOT mark a debug/test image permanent (`image-confirm`) prematurely.**
  Flash an unproven image as a **test boot only** (`image-test` + `reset`, no
  `image-confirm`). A test image that crashes/reboots is automatically reverted
  by MCUboot to the previous good image. The moment you confirm it (or
  `image-confirm` its hash while it's the pending image), you make it permanent
  and remove that safety net — a boot-looping or pre-USB-hanging confirmed image
  can no longer be rolled back over USB. Only `image-confirm` an image you have
  verified is healthy and reachable.


## Flashing

**Normal workflow: BLE DFU from the Raspberry Pi** — see "BLE DFU workflow (via
the Raspberry Pi)" below.

### USB DFU via mcu-manager

Use this if xiao_ble is connected to USB, or BLE DFU does not work (e.g. broken firmware). The xiao_ble USB
connection is VID:PID `2fe3:0004`, typically `/dev/ttyACM2`.

```bash
# 1. Upload to slot 1
nrfutil mcu-manager serial --serial-port /dev/ttyACM2 --timeout 60 \
    image-upload --firmware build/dfu_application.zip

# 2. Read the new slot-1 hash
nrfutil mcu-manager serial --serial-port /dev/ttyACM2 image-list

# 3. Mark for test, then reset (xiao_ble re-enumerates USB; wait ~5s)
nrfutil mcu-manager serial --serial-port /dev/ttyACM2 image-test --hash <slot1-hash>
nrfutil mcu-manager serial --serial-port /dev/ttyACM2 reset

# 4. Confirm — otherwise MCUboot reverts on the next reset
nrfutil mcu-manager serial --serial-port /dev/ttyACM2 image-confirm --hash <slot0-hash-after-swap>
```

The firmware must keep `CONFIG_MCUMGR=y`, `CONFIG_MCUMGR_TRANSPORT_UART=y`,
`CONFIG_MCUMGR_GRP_IMG=y`, `CONFIG_MCUMGR_GRP_OS=y`, `CONFIG_IMG_MANAGER=y` (and
flash/CBOR/CRC deps) so the *next* update has an SMP server to talk to.

## BLE client (`tools/ble-client`)

A Go CLI tool that connects to the xiao_ble over BLE. It is cross-platform; the
pairing layer differs per OS (see `pairing_linux.go` / `pairing_other.go`):

- **Linux (BlueZ):** uses BlueZ's default adapter (`hci0`): on the dev machine
  an nrf52840dk running `hci_usb`, on the Raspberry Pi flashing host (see "BLE
  DFU workflow") the Pi's built-in radio. Pairing is handled automatically using the
  passkey from the `FLEXIT_CS60_CONTROL_BLE_KEY` environment variable (a 6-digit
  number) — the same variable the firmware bakes in at build time. Every
  connecting subcommand exits with an error if it is unset (`scan` does not
  pair, so it does not need it).
- **macOS (CoreBluetooth) / Windows (WinRT):** uses the machine's built-in
  Bluetooth radio — no separate HCI dongle. The OS owns pairing: on first
  encrypted access it shows a system pairing dialog where you type the 6-digit
  passkey by hand, so `FLEXIT_CS60_CONTROL_BLE_KEY` is *not* used. Building
  requires cgo + the platform SDK (e.g. Xcode on macOS), so it must be built on
  that OS — it cannot be cross-compiled from Linux.

```bash
cd tools/ble-client
go build -o ble-client .

# Required for any subcommand that pairs (state, mode, flash, …):
export FLEXIT_CS60_CONTROL_BLE_KEY=444999
```

### Subcommands

```bash
# Continuous capture: stream live RS485 bytes to file (Ctrl-C to stop)
./ble-client stream rs485_live.bin

# Print decoded panel state (MODE, temps, percentages, runtime counters)
./ble-client state                  # compact one-line-per-field
./ble-client state --human-friendly # verbose multiline

# Queue a CMD_MODE change via the XIAO's Modbus slave
#   0=Stop, 1=Min, 2=Normal, 3=Max
# The XIAO raises coil 0 / reg 0; CS60 picks it up on its next FC01 poll,
# reads reg 0 via FC03, and broadcasts FC65 to ack — same cycle as a
# physical panel press. Requires the XIAO to be at a CS60-registered
# slave address (i.e. present and responding when CS60 last booted).
./ble-client mode 1

# Zigbee factory reset — leave the current network, clear ZBOSS NVRAM, and
# reboot. A clean NVRAM boots as DEVICE_FIRST_START, which auto-starts BDB
# network steering, so the XIAO becomes joinable again. Use this to re-pair
# with a new coordinator (e.g. move from the test coordinator to HA/ZHA):
# open ZHA "Add device" (permit join) first, then run this. The BLE link drops
# as the device reboots (~1-5 s) — expected.
./ble-client zbreset

# Scan and print RSSI (useful to check signal before flashing)
./ble-client scan

# Flash firmware over BLE via SMP (upload → test-mark → reset)
./ble-client flash build/flexit-cs60-control/zephyr/zephyr.signed.bin

# Confirm after the test boot is verified healthy. Refuses unless <hash-hex>
# is the active image in slot 0. There is no timeout: an unconfirmed image
# keeps running until the next reset, which reverts to the previous image.
./ble-client confirm <hash-hex>

# List firmware images in slot 0 and slot 1
./ble-client list
```

`stream` writes raw RS485 bytes. Use `xxd` to inspect:
`xxd rs485_live.bin | head`

`stream` sends a `"stop"` command before disconnecting so the firmware cleanly
disables forwarding.

### BLE DFU workflow (via the Raspberry Pi)

BLE flashing is done from a Raspberry Pi Zero next to the XIAO, reachable over
SSH (arm64 Raspberry Pi OS). Its hostname is deliberately not in the repo: take
it from memory outside the repo, or ask the user — written as `$PI_HOST` below.
It runs `ble-client` with its built-in Bluetooth radio and is already bonded
with the device. Build on the dev machine, copy the binary and image over, and run
`ble-client` on the Pi. Use `ssh -o BatchMode=yes` so a missing key fails
instead of hanging on a prompt.

```bash
# 1. Build on the dev machine (FLEXIT_CS60_CONTROL_BLE_KEY = the device's key,
#    see "Environment"; use --pristine if the key changed since the last build,
#    since it is only picked up when CMake configures)
source ~/ncs/v3.4.0/zephyr/zephyr-env.sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --build-dir build

# 2. Cross-compile ble-client for the Pi (only when tools/ble-client changed)
(cd tools/ble-client && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o /tmp/ble-client-arm64 .)
scp /tmp/ble-client-arm64 $PI_HOST:ble-client

# 3. Copy the signed image to the Pi
scp build/flexit-cs60-control/zephyr/zephyr.signed.bin $PI_HOST:

# 4. Flash as a TEST BOOT (upload -> test-mark -> reset; never confirms).
#    Prints the slot-1 hash.
ssh $PI_HOST \
    "FLEXIT_CS60_CONTROL_BLE_KEY=$FLEXIT_CS60_CONTROL_BLE_KEY ./ble-client flash zephyr.signed.bin"

# 5. Verify the test boot. MCUboot swaps from external flash first; the scan
#    just waits until the device advertises again.
#    - list:    slot 0 = new hash, active, confirmed:false
#    - state:   FC10 frames counting, CRCERR ~0, SLV_FC01 > 0 (CS60 polls us)
#    - zbstate: JOINED
ssh $PI_HOST "export FLEXIT_CS60_CONTROL_BLE_KEY=$FLEXIT_CS60_CONTROL_BLE_KEY; \
    ./ble-client list; ./ble-client state; ./ble-client zbstate"

# 6. Have the user check Home Assistant, then — only with their go-ahead —
#    confirm (refuses unless the hash is the active slot-0 image):
ssh $PI_HOST \
    "FLEXIT_CS60_CONTROL_BLE_KEY=$FLEXIT_CS60_CONTROL_BLE_KEY ./ble-client confirm <hash printed by flash>"
```

If the test image misbehaves, do not confirm: `./ble-client reboot` (or a power
cycle) makes MCUboot revert to the previous confirmed image.

Running `ble-client` directly on the dev machine (nrf52840dk HCI adapter) works
the same way, without the ssh/scp steps.

The firmware must keep `CONFIG_MCUMGR_TRANSPORT_BT=y` and
`CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN=y` so the BLE SMP server is active.
