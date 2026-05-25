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
import signal
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

# macOS CoreBluetooth caches "not advertising" for a name-based scan filter
# for many seconds after a disconnect, so the post-DFU/confirm console connect
# fails even though the device is in fact advertising. Cache the device
# address after the first successful scan and prefer it on subsequent runs;
# this bypasses the cache entirely.
_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDR_CACHE    = os.path.join(_PROJECT_ROOT, "run", "device_address.txt")


def _load_cached_address() -> str | None:
    try:
        with open(ADDR_CACHE) as f:
            addr = f.read().strip()
            return addr or None
    except OSError:
        return None


def _save_cached_address(addr: str) -> None:
    try:
        os.makedirs(os.path.dirname(ADDR_CACHE), exist_ok=True)
        with open(ADDR_CACHE, "w") as f:
            f.write(addr)
    except OSError:
        pass


def rx(characteristic, data: bytearray) -> None:
    """Print NUS notification payload as UTF-8; fall back to hex on decode error."""
    try:
        print(data.decode("utf-8"), end="", flush=True)
    except UnicodeDecodeError:
        print(" ".join(f"{b:02X}" for b in data), flush=True)


async def run(name: str, address: str | None, scan_timeout: float) -> None:
    # Route SIGTERM (and reinforce SIGINT) into asyncio cancellation so the
    # `async with BleakClient(...)` block below gets a chance to disconnect
    # the BLE link cleanly when this script is killed by a parent process
    # (e.g. the harness's background-task lifecycle) rather than Ctrl-C.
    # Without this, the peripheral can be left thinking the central is still
    # connected, blocking the next connect attempt until the link supervision
    # timeout fires.
    loop      = asyncio.get_running_loop()
    main_task = asyncio.current_task()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, main_task.cancel)
        except NotImplementedError:
            pass  # not supported on Windows

    device = None

    if address:
        device = await BleakScanner.find_device_by_address(address,
                                                            timeout=scan_timeout)
        if not device:
            print(f"error: device with address '{address}' not found.")
            sys.exit(1)
    else:
        # 1. Try the cached address from a previous successful run first —
        #    this avoids macOS's name-scan cache which can stay stale for
        #    many seconds after a disconnect.
        cached = _load_cached_address()
        if cached:
            print(f"Trying cached address {cached} (up to 5 s)…")
            device = await BleakScanner.find_device_by_address(cached,
                                                                timeout=5)
            if not device:
                print("  cached address not seen — falling back to name scan.")

        # 2. Name scan with retries.
        if not device:
            attempts = 3
            for attempt in range(1, attempts + 1):
                if attempt == 1:
                    print(f"Scanning for '{name}' (up to {scan_timeout:.0f} s)…")
                else:
                    await asyncio.sleep(2)
                    print(f"  not found yet — retrying ({attempt}/{attempts})…")
                device = await BleakScanner.find_device_by_filter(
                    lambda d, _: d.name == name,
                    timeout=scan_timeout,
                )
                if device:
                    break

        if not device:
            print(f"error: '{name}' not found after {attempts} scans.\n"
                  f"       If the device just disconnected from another BLE client,\n"
                  f"       macOS may still be caching stale state. Try:\n"
                  f"         1. Wait ~10 seconds and rerun.\n"
                  f"         2. Clear macOS's pairing entry under System Settings →\n"
                  f"            Bluetooth → (i) next to '{name}' → Forget.\n"
                  f"         3. Power-cycle the XIAO if nothing else works.")
            sys.exit(1)

        _save_cached_address(device.address)

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
