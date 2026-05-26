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

# Flash
west flash --build-dir build

# Kconfig menu
west build --build-dir build -t menuconfig

# Clean
west build --build-dir build -t clean
```
