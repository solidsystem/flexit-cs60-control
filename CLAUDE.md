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
- SDK: `/opt/nordic/ncs/v3.3.0`
- Toolchain: `/opt/nordic/ncs/toolchains/0c0f19d91c`
- West: `/opt/nordic/ncs/toolchains/0c0f19d91c/bin/west`
- Board: `xiao_ble/nrf52840`

## Environment

Set these before running west commands:

```bash
export TOOLCHAIN=/opt/nordic/ncs/toolchains/0c0f19d91c
export PATH=$TOOLCHAIN/bin:$TOOLCHAIN/usr/bin:$TOOLCHAIN/opt/bin:$TOOLCHAIN/nrfutil/bin:$TOOLCHAIN/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=$TOOLCHAIN/opt/zephyr-sdk
```

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

## Flashing workflows

Single sysbuild dir `build/` serves both flash paths:

- **SWD via Nordic DK J-Link OB** (serial `1050279550`) — used once to install MCUboot + the first signed app, or whenever the bootloader needs reflashing. `west flash --build-dir build` flashes both images per `domains.yaml flash_order: [mcuboot, flexitMC3]`. For faster post-bootloader iteration over SWD use `--domain flexitMC3` to flash only the signed app.
- **USB DFU over the XIAO's own USB-C** — `tools/usb_dfu_flash.sh build` uploads `build/flexitMC3/zephyr/zephyr.signed.bin` via `nrfutil mcu-manager` (Nordic's SMP client, ships in the NCS toolchain — no extra install). Marks for test boot, resets; MCUboot swaps on the reset. After successful boot, run `nrfutil mcu-manager serial --serial-port <port> image-confirm` to make sticky. **Note:** uses the `mcu-manager` plugin (SMP protocol). Do **not** confuse with the `nrf5sdk-tools dfu` plugin, which speaks the legacy nRF5 SDK DFU protocol and is incompatible with MCUboot.

The nRF Connect VS Code extension's Flash button is hard-wired to `west flash` (SWD runners) — it does **not** do USB DFU. For USB DFU use the VS Code task `nRF: Build & USB DFU Flash` (set as default test task → `Cmd+Shift+T`).

## VS Code Tasks

Use `Ctrl+Shift+B` (or `Cmd+Shift+B` on Mac) to run the default build task, or open the Command Palette and run "Tasks: Run Task" to see all available tasks:

- **nRF: Build** — incremental build (default build task)
- **nRF: Build (Pristine)** — clean rebuild
- **nRF: Flash** — flash to device
- **nRF: Build & Flash** — build then flash
- **nRF: Menuconfig** — open Kconfig menu
- **nRF: Clean** — clean build artifacts
- **nRF: Flash (SWD, app only)** — flash only the signed app via J-Link (skips MCUboot)
- **nRF: USB DFU Flash** — upload signed image over USB CDC-ACM via `mcumgr`, test-boot, reset
- **nRF: Build & USB DFU Flash** — chained build + USB upload (default test task)
- **nRF: USB DFU Confirm** — mark the currently-running image as confirmed (use after a test boot succeeds, otherwise the next reset reverts)
