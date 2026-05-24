#!/usr/bin/env python3
"""
BLE console terminal for flexitMC3 (Nordic UART Service).

Scans for the device, connects, and streams NUS TX notifications to stdout.
macOS handles the passkey dialog natively when the peripheral requests L3
security — enter the passkey shown on the USB console (default: 444999).

Usage:
    python3 tools/ble_console.py
    python3 tools/ble_console.py --name flexitMC3
    python3 tools/ble_console.py --address AA:BB:CC:DD:EE:FF

Requires:
    pip3 install bleak
"""

import argparse
import asyncio
import os
import sys

# Prefer the locally vendored bleak (tools/lib/) so no system install is needed.
_lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
if os.path.isdir(_lib) and _lib not in sys.path:
    sys.path.insert(0, _lib)

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("error: bleak not found.  Run: pip3 install --target tools/lib bleak")
    sys.exit(1)

# Nordic UART Service UUIDs
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # peripheral → central


def rx(characteristic, data: bytearray) -> None:
    """Print NUS notification payload as UTF-8; fall back to hex on decode error."""
    try:
        print(data.decode("utf-8"), end="", flush=True)
    except UnicodeDecodeError:
        print(" ".join(f"{b:02X}" for b in data), flush=True)


async def run(name: str, address: str | None, scan_timeout: float) -> None:
    if address:
        device = await BleakScanner.find_device_by_address(address,
                                                            timeout=scan_timeout)
        if not device:
            print(f"error: device with address '{address}' not found.")
            sys.exit(1)
    else:
        print(f"Scanning for '{name}' (up to {scan_timeout:.0f} s)…")
        device = await BleakScanner.find_device_by_filter(
            lambda d, _: d.name == name,
            timeout=scan_timeout,
        )
        if not device:
            print(f"error: '{name}' not found.  Is the device advertising?")
            sys.exit(1)

    print(f"Found {device.name}  [{device.address}]  — connecting…")

    async with BleakClient(device) as client:
        print("Connected.")
        print("macOS will prompt for the passkey if this is the first pairing.")
        print("Streaming NUS console output — Ctrl-C to quit.\n")

        await client.start_notify(NUS_TX_UUID, rx)

        try:
            # Keep the event loop alive; the notification callback does the work.
            while client.is_connected:
                await asyncio.sleep(0.5)
        except (asyncio.CancelledError, KeyboardInterrupt):
            pass
        finally:
            if client.is_connected:
                await client.stop_notify(NUS_TX_UUID)

    print("\nDisconnected.")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", default="flexitMC3",
                   help="BLE device name to scan for (default: flexitMC3)")
    p.add_argument("--address",
                   help="Skip scanning and connect to this BLE address directly")
    p.add_argument("--timeout", type=float, default=15.0,
                   help="Scan timeout in seconds (default: 15)")
    args = p.parse_args()

    try:
        asyncio.run(run(args.name, args.address, args.timeout))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
