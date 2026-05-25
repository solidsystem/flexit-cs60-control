#!/usr/bin/env bash
# Thin wrapper around `west build` that sets the NCS toolchain environment
# documented in CLAUDE.md, then runs the standard sysbuild invocation.
# Extra args are forwarded to `west build` (e.g. --pristine, --domain X).
set -euo pipefail

export TOOLCHAIN=/opt/nordic/ncs/toolchains/0c0f19d91c
export PATH="$TOOLCHAIN/bin:$TOOLCHAIN/usr/bin:$TOOLCHAIN/opt/bin:$TOOLCHAIN/nrfutil/bin:$TOOLCHAIN/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TOOLCHAIN/opt/zephyr-sdk"

cd "$(dirname "$0")/.."
exec west build -b xiao_ble/nrf52840 --sysbuild --build-dir build "$@"
