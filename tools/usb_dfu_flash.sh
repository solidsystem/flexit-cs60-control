#!/usr/bin/env bash
# Upload the signed app from a sysbuild output directory to the XIAO BLE over
# its native USB CDC-ACM using `nrfutil mcu-manager` (Nordic's SMP client),
# then mark the new image for test-boot and reset the device. MCUboot performs
# the swap on the reset.
#
# Note: this uses the `mcu-manager` plugin of nrfutil (SMP/MCUmgr protocol),
# NOT `nrf5sdk-tools dfu` (legacy nRF5 SDK DFU protocol, incompatible with
# MCUboot).
#
# Usage: tools/usb_dfu_flash.sh [BUILD_DIR]
#   BUILD_DIR defaults to "build". Must contain
#   <BUILD_DIR>/flexitMC3/zephyr/zephyr.signed.bin (produced by sysbuild with
#   CONFIG_BOOTLOADER_MCUBOOT=y).

set -euo pipefail

BUILD_DIR="${1:-build}"
IMAGE="${BUILD_DIR}/flexitMC3/zephyr/zephyr.signed.bin"

# Pick up nrfutil from the NCS toolchain if not already on PATH.
if ! command -v nrfutil >/dev/null 2>&1; then
    TOOLCHAIN=/opt/nordic/ncs/toolchains/0c0f19d91c
    export PATH="$TOOLCHAIN/bin:$TOOLCHAIN/nrfutil/bin:$PATH"
fi

if ! command -v nrfutil >/dev/null 2>&1; then
    echo "error: nrfutil not found." >&2
    exit 1
fi

if ! nrfutil list 2>/dev/null | grep -q '^mcu-manager '; then
    echo "error: nrfutil 'mcu-manager' plugin not installed." >&2
    echo "       install with: nrfutil install mcu-manager" >&2
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "error: signed image not found at $IMAGE" >&2
    echo "       build first, then re-run this script." >&2
    exit 1
fi

# Pick a /dev/cu.usbmodem* that is NOT the Nordic DK's J-Link OB.
# The DK exposes virtual COM ports as /dev/cu.usbmodem<JLINKSN>{1,3};
# the XIAO's own CDC-ACM enumerates with a different serial.
JLINK_SN="1050279550"

mapfile -t CANDIDATES < <(ls /dev/cu.usbmodem* 2>/dev/null | grep -v "$JLINK_SN" || true)
if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    echo "error: no XIAO CDC-ACM port found." >&2
    echo "       plug the XIAO's own USB-C into the Mac and ensure the SWD probe is disconnected." >&2
    echo "       all USB modem devices currently visible:" >&2
    ls /dev/cu.usbmodem* 2>/dev/null | sed 's/^/         /' >&2 || echo "         (none)" >&2
    exit 1
fi
PORT="${CANDIDATES[0]}"
echo "Using XIAO port: $PORT"

# macOS only permits one process to hold a /dev/cu.usbmodem* at a time.
# Check for an existing holder (commonly the VS Code serial monitor or `screen`).
HOLDER=$(lsof -t "$PORT" "${PORT/cu./tty.}" 2>/dev/null | head -1 || true)
if [[ -n "$HOLDER" ]]; then
    HOLDER_CMD=$(ps -o comm= -p "$HOLDER" 2>/dev/null || echo "<unknown>")
    cat >&2 <<EOF
error: $PORT is already open by PID $HOLDER ($HOLDER_CMD).
       macOS allows only one process on a serial port at a time. Close it
       and re-run, e.g.:
         - VS Code Serial Monitor: click "Stop Monitoring" in the tab
         - nRF Connect extension: disconnect the terminal panel
         - 'screen': press Ctrl-A, then K, then Y to kill the session
         - last resort: kill $HOLDER
EOF
    exit 1
fi

echo "==> Computing image hash locally"
# image-hash prints "<hash>  <path>" to stdout; grab the hash word.
HASH=$(nrfutil mcu-manager image-hash --firmware "$IMAGE" | awk 'NF>=1{print $1; exit}')
if [[ -z "$HASH" ]]; then
    echo "error: failed to compute image hash" >&2
    exit 1
fi
echo "    hash: $HASH"

echo "==> Uploading $(basename "$IMAGE") ($(wc -c <"$IMAGE" | tr -d ' ') bytes)"
nrfutil mcu-manager serial --serial-port "$PORT" image-upload --firmware "$IMAGE"

echo "==> Marking image for test boot"
nrfutil mcu-manager serial --serial-port "$PORT" image-test --hash "$HASH"

echo "==> Reset (MCUboot will swap on this boot)"
nrfutil mcu-manager serial --serial-port "$PORT" reset

cat <<EOF

Done. The XIAO is rebooting; MCUboot is swapping slot0 <-> slot1 and will run
the new image. If the new image boots correctly and you want to keep it, run
task "nRF: USB DFU Confirm".

Otherwise, the next reset will revert to the previous image.
EOF
