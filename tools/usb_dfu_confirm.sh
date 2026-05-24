#!/usr/bin/env bash
# Confirm the currently-running image on the XIAO BLE so MCUboot won't revert
# to the previous slot on next reset. Run this after a USB DFU flash has
# test-booted successfully and you want to keep it.

set -euo pipefail

# Pick up nrfutil from the NCS toolchain if not already on PATH.
if ! command -v nrfutil >/dev/null 2>&1; then
    TOOLCHAIN=/opt/nordic/ncs/toolchains/0c0f19d91c
    export PATH="$TOOLCHAIN/bin:$TOOLCHAIN/nrfutil/bin:$PATH"
fi

if ! command -v nrfutil >/dev/null 2>&1; then
    echo "error: nrfutil not found." >&2
    exit 1
fi

# Pick a /dev/cu.usbmodem* that is NOT the Nordic DK's J-Link OB.
JLINK_SN="1050279550"
mapfile -t CANDIDATES < <(ls /dev/cu.usbmodem* 2>/dev/null | grep -v "$JLINK_SN" || true)
if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    echo "error: no XIAO CDC-ACM port found." >&2
    echo "       plug the XIAO's own USB-C into the Mac and ensure the SWD probe is disconnected." >&2
    exit 1
fi
PORT="${CANDIDATES[0]}"
echo "Using XIAO port: $PORT"

# macOS only permits one process to hold a /dev/cu.usbmodem* at a time.
HOLDER=$(lsof -t "$PORT" "${PORT/cu./tty.}" 2>/dev/null | head -1 || true)
if [[ -n "$HOLDER" ]]; then
    HOLDER_CMD=$(ps -o comm= -p "$HOLDER" 2>/dev/null || echo "<unknown>")
    cat >&2 <<EOF
error: $PORT is already open by PID $HOLDER ($HOLDER_CMD).
       Close it (e.g. VS Code Serial Monitor "Stop Monitoring") and re-run.
EOF
    exit 1
fi

echo "==> Confirming currently-running image"
nrfutil mcu-manager serial --serial-port "$PORT" image-confirm

echo "Done. MCUboot will boot this image from now on."
