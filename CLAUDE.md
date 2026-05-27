# flexitMC3

## Project goal

The project goal is to create a smarthouse device that allows monitoring and controlling 
the flexit ventilation system. The smarthouse controller will connect to xiao_ble with
either bluetooth or zigbee (not decided which to implement yet).

## hardware info

Zephyr/nRF Connect SDK application for Seeed XIAO BLE (nRF52840) with MCUboot
(https://wiki.seeedstudio.com/XIAO_BLE/).

The board sits on a **Seeed XIAO-RS485-Expansion-Board**
(https://wiki.seeedstudio.com/XIAO-RS485-Expansion-Board/). 

The RS485 transceiver is connected (via rj12 connector) to **flexit cl60** control panel, 
which is connected to **flexit cs60**. 

For documentation on mentioned flexit hardware components, read pdf files in docs/ directory.

This project is very similar to the solution we want to implement:
- https://github.com/MSkjel/esphome-flexit-modbus-server/tree/main

## Toolchain

- NCS version: v3.3.0
- SDK: `~/ncs/v3.3.0`
- Board: `xiao_ble/nrf52840`

When creating helper scripts/software, do it in golang if possible.

## Environment

Commands must be run with the NCS toolchain. The pattern is:

```bash
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- <command>
```

If needed, read about managing and initializing SDK toolchain at https://docs.nordicsemi.com/bundle/nrfutil/page/nrfutil-toolchain-manager/nrfutil-toolchain-manager.html

## Common Commands

```bash
# Build
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --build-dir build

# Pristine build (clean slate)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --pristine --build-dir build

# Kconfig menu
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
    west build --build-dir build -t menuconfig

# Clean
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
    west build --build-dir build -t clean
```

## Flashing

**Normal workflow: BLE DFU** — see the `BLE client` section below.

**Do not use `west flash`** — it targets the J-Link on the nrf52840dk, not the xiao_ble.

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

A Go CLI tool that connects to the xiao_ble over BLE using an nrf52840dk running
`hci_usb` as the HCI adapter (appears as `hci0` in BlueZ). Handles BLE pairing
automatically with fixed passkey `444999`.

```bash
cd tools/ble-client
go build -o ble-client .
```

### Subcommands

```bash
# One-shot snapshot: fetch the last 2 KiB of recorded RS485 traffic
./ble-client fetch rs485_capture.bin

# Continuous capture: stream live RS485 bytes to file (Ctrl-C to stop)
./ble-client stream rs485_live.bin

# Scan and print RSSI (useful to check signal before flashing)
./ble-client scan

# Flash firmware over BLE via SMP (upload → test-mark → reset)
./ble-client flash build/flexitMC/zephyr/zephyr.signed.bin

# Confirm after reboot (run within ~60s of flash completing)
./ble-client confirm <hash-hex>

# List firmware images in slot 0 and slot 1
./ble-client list
```

Both `fetch` and `stream` write raw RS485 bytes. Use `xxd` to inspect:
`xxd rs485_capture.bin | head`

`fetch` is interrupted with partial data written if Ctrl-C is pressed mid-transfer.
`stream` sends a `"stop"` command before disconnecting so the firmware cleanly
disables forwarding.

### BLE DFU workflow

```bash
# 1. Build
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
    west build -b xiao_ble/nrf52840 --sysbuild --build-dir build

# 2. Flash over BLE (prints the hash and the confirm command to run next)
cd tools/ble-client
./ble-client flash ../../build/flexitMC/zephyr/zephyr.signed.bin

# 3. Confirm after device reboots (~10s)
./ble-client confirm <hash printed by flash>
```

**Tip:** If connection fails with `le-connection-abort-by-local`, check signal strength
with `./ble-client scan` first (need -75 dBm or better). If signal is fine, restart
BlueZ: `sudo systemctl restart bluetooth`.

The firmware must keep `CONFIG_MCUMGR_TRANSPORT_BT=y` and
`CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN=y` so the BLE SMP server is active.
