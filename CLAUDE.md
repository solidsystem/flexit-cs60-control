# flexitMC3

Zephyr/nRF Connect SDK application for Seeed XIAO BLE (nRF52840) with MCUboot
(https://wiki.seeedstudio.com/XIAO_BLE/).

The board sits on a **Seeed XIAO-RS485-Expansion-Board**
(https://wiki.seeedstudio.com/XIAO-RS485-Expansion-Board/). 

The RS485 transceiver is connected (via rj12 connector) to **flexit cl60** control panel, 
which is connected to **flexit cs60**. 

The project goal is to be a programmable modbus interface to the flexit control panel, 
without using a flexit cl66 device.

For documentation on mentioned flexit hardware components, read pdf files docs directory.

This project is very similar to the solution we want to implement:
- https://github.com/MSkjel/esphome-flexit-modbus-server/tree/main

## Toolchain

- NCS version: v3.3.0
- SDK: `~/ncs/v3.3.0`
- Board: `xiao_ble/nrf52840`

## Environment

To run commands in shell:
- source this script `~/ncs/v3.3.0/zephyr/zephyr-env.sh`
- use this command prefix to set required toolchain environment: `nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 --shell`

If needed, read about managing and initializing SDK toolchain at https://docs.nordicsemi.com/bundle/nrfutil/page/nrfutil-toolchain-manager/nrfutil-toolchain-manager.html

## Common Commands

```bash
# Build
west build -b xiao_ble/nrf52840 --sysbuild --build-dir build

# Pristine build (clean slate)
west build -b xiao_ble/nrf52840 --sysbuild --pristine --build-dir build

# Kconfig menu
west build --build-dir build -t menuconfig

# Clean
west build --build-dir build -t clean
```

## Flashing the xiao_ble (USB DFU via mcu-manager)

The xiao_ble has **no debug programmer attached** — it is connected by USB only
(VID:PID `2fe3:0004`, typically `/dev/ttyACM2`). It runs MCUboot with an MCUmgr
SMP server over USB CDC ACM, so firmware is delivered as a DFU upload.

**Do not use `west flash`** — it would target a J-Link belonging to a separate
nrf52840dk board that may be on the bus, not the xiao_ble.

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

The flashed firmware must keep `CONFIG_MCUMGR=y`, `CONFIG_MCUMGR_TRANSPORT_UART=y`,
`CONFIG_MCUMGR_GRP_IMG=y`, `CONFIG_MCUMGR_GRP_OS=y`, `CONFIG_IMG_MANAGER=y` (and
flash/CBOR/CRC deps) so the *next* update has an SMP server to talk to.
