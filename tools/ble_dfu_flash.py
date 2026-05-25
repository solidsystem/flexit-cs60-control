#!/usr/bin/env python3
"""
BLE DFU flash for flexitMC3 (SMP/MCUmgr over Bluetooth LE).

Uploads the signed image to the device, marks it for test boot, and resets.
MCUboot swaps the image on the next boot. Uses the Mac's built-in Bluetooth
via CoreBluetooth (no external HCI dongle needed).

macOS handles the passkey dialog when the device requests L3 security.
Enter the fixed passkey shown on the USB console when prompted.

Usage:
    python3 tools/ble_dfu_flash.py [BUILD_DIR]       # flash and test-boot
    python3 tools/ble_dfu_flash.py --confirm          # confirm running image
    BUILD_DIR defaults to "build"

Requires:
    pip3 install --target tools/lib bleak cbor2       # already in tools/lib
"""

import argparse
import asyncio
import os
import struct
import subprocess
import sys

_lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
if os.path.isdir(_lib) and _lib not in sys.path:
    sys.path.insert(0, _lib)

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("error: bleak not found.  Run: pip3 install --target tools/lib bleak cbor2")
    sys.exit(1)

try:
    import cbor2
except ImportError:
    print("error: cbor2 not found.  Run: pip3 install --target tools/lib cbor2")
    sys.exit(1)

DEVICE_NAME   = "flexitMC3"
SMP_CHAR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"

# See ble_console.py for the rationale — macOS name-scan cache work-around.
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

OP_READ  = 0
OP_WRITE = 2

GRP_OS    = 0
GRP_IMAGE = 1

ID_IMAGE_STATE  = 0
ID_IMAGE_UPLOAD = 1
ID_OS_RESET     = 5

# Safe data payload per SMP frame — fits in a 160-byte BLE ATT write
# (128 data + 8 SMP header + ~24 CBOR overhead on the first chunk).
CHUNK_SIZE = 128


# ── SMP framing ────────────────────────────────────────────────────────────
#
# Header byte 0 packs three fields (little-endian bitfields on device):
#   bits 0-2: op (MGMT_OP_READ=0, MGMT_OP_WRITE=2, ...)
#   bits 3-4: version (SMP_MCUMGR_VERSION_1=0, SMP_MCUMGR_VERSION_2=1)
#   bits 5-7: reserved (0)
# We use V2 so the device returns the group-specific error in `err: {group, rc}`
# instead of collapsing every IMG-group error into the V1 MGMT_ERR_EUNKNOWN.

SMP_VERSION = 1


def smp_pack(op, group, id_, seq, payload_map):
    payload = cbor2.dumps(payload_map)
    op_byte = (op & 0x07) | ((SMP_VERSION & 0x03) << 3)
    return struct.pack(">BBHHBB", op_byte, 0, len(payload), group, seq, id_) + payload


def smp_unpack(data):
    op, _flags, length, group, seq, id_ = struct.unpack(">BBHHBB", data[:8])
    cbor = cbor2.loads(data[8: 8 + length]) if length else {}
    return op, group, id_, seq, cbor


# ── SMP client over BLE notifications ──────────────────────────────────────

class SMPClient:
    def __init__(self, client: BleakClient):
        self._ble    = client
        self._seq    = 0
        self._buf    = bytearray()
        self._expect = 0
        self._future: asyncio.Future | None = None

    async def start(self):
        await self._ble.start_notify(SMP_CHAR_UUID, self._on_notify)

    async def stop(self):
        if self._ble.is_connected:
            await self._ble.stop_notify(SMP_CHAR_UUID)

    def _on_notify(self, _char, data: bytearray):
        self._buf.extend(data)
        if len(self._buf) >= 8 and self._expect == 0:
            _, _, length, _, _, _ = struct.unpack(">BBHHBB", bytes(self._buf[:8]))
            self._expect = 8 + length
        if self._expect and len(self._buf) >= self._expect:
            frame = bytes(self._buf[: self._expect])
            self._buf    = self._buf[self._expect:]
            self._expect = 0
            if self._future and not self._future.done():
                self._future.set_result(frame)

    async def request(self, op, group, id_, payload_map, timeout=30.0):
        loop = asyncio.get_running_loop()
        self._future = loop.create_future()
        frame = smp_pack(op, group, id_, self._seq, payload_map)
        self._seq = (self._seq + 1) % 256
        await self._ble.write_gatt_char(SMP_CHAR_UUID, frame, response=False)
        raw = await asyncio.wait_for(self._future, timeout=timeout)
        _, _, _, _, cbor = smp_unpack(raw)
        return cbor


# ── Response handling ──────────────────────────────────────────────────────
#
# SMP has two error formats:
#   V1 (legacy):  {"rc": N}                       — used when request header
#                                                    sets version=0 AND device
#                                                    has CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL=y
#   V2 (newer):   {"err": {"group": G, "rc": N}}  — used when request version=1
#                                                    OR when V1 support is off
#
# The IMG_UPLOAD success response is always {"rc": 0, "off": <next>} (V1 form).
# We accept either format on errors to stay forward-compatible.

_MGMT_ERR = {
    0:  "EOK", 1:  "EUNKNOWN",       2:  "ENOMEM",         3:  "EINVAL",
    4:  "ETIMEOUT", 5:  "ENOENT",    6:  "EBADSTATE",      7:  "EMSGSIZE",
    8:  "ENOTSUP",  9:  "ECORRUPT",  10: "EBUSY",          11: "EACCESSDENIED",
    12: "UNSUPPORTED_TOO_OLD", 13: "UNSUPPORTED_TOO_NEW",
}

_IMG_MGMT_ERR = {
    0:  "OK",                       1:  "UNKNOWN",
    2:  "FLASH_CONFIG_QUERY_FAIL",  3:  "NO_IMAGE",
    4:  "NO_TLVS",                  5:  "INVALID_TLV",
    6:  "TLV_MULTIPLE_HASHES",      7:  "TLV_INVALID_SIZE",
    8:  "HASH_NOT_FOUND",           9:  "NO_FREE_SLOT",
    10: "FLASH_OPEN_FAILED",        11: "FLASH_READ_FAILED",
    12: "FLASH_WRITE_FAILED",       13: "FLASH_ERASE_FAILED",
    14: "INVALID_SLOT",             15: "NO_FREE_MEMORY",
    16: "FLASH_CONTEXT_ALREADY_SET",17: "FLASH_CONTEXT_NOT_SET",
    18: "FLASH_AREA_DEVICE_NULL",   19: "INVALID_PAGE_OFFSET",
    20: "INVALID_OFFSET",           21: "INVALID_LENGTH",
    22: "INVALID_IMAGE_HEADER",     23: "INVALID_IMAGE_HEADER_MAGIC",
    24: "INVALID_HASH",             25: "INVALID_FLASH_ADDRESS",
    26: "VERSION_GET_FAILED",       27: "CURRENT_VERSION_IS_NEWER",
    28: "IMAGE_ALREADY_PENDING",    29: "INVALID_IMAGE_VECTOR_TABLE",
    30: "INVALID_IMAGE_TOO_LARGE",  31: "INVALID_IMAGE_DATA_OVERRUN",
    32: "IMAGE_CONFIRMATION_DENIED",33: "IMAGE_SETTING_TEST_TO_ACTIVE_DENIED",
}


def _check_rc(resp):
    """Return (rc, human_string).  rc=0 means success."""
    if "rc" in resp and resp["rc"] != 0:
        return resp["rc"], _MGMT_ERR.get(resp["rc"], "?")
    if "err" in resp and isinstance(resp["err"], dict):
        rc = resp["err"].get("rc", -1)
        grp = resp["err"].get("group", -1)
        name = _IMG_MGMT_ERR.get(rc, "?") if grp == 1 else _MGMT_ERR.get(rc, "?")
        return rc, f"group={grp} {name}"
    return 0, ""


# ── Operations ─────────────────────────────────────────────────────────────

def image_hash(image_path: str) -> str:
    """Use nrfutil to compute the MCUboot-compatible image hash."""
    for candidate in [
        "/opt/nordic/ncs/toolchains/0c0f19d91c/nrfutil/bin/nrfutil",
        "/opt/nordic/ncs/toolchains/0c0f19d91c/bin/nrfutil",
        "nrfutil",
    ]:
        try:
            out = subprocess.check_output(
                [candidate, "mcu-manager", "image-hash", "--firmware", image_path],
                stderr=subprocess.DEVNULL,
            ).decode()
            return out.split()[0]
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    sys.exit("error: nrfutil not found — cannot compute image hash")


async def upload(smp: SMPClient, image: bytes, hash_hex: str):
    total  = len(image)
    offset = 0
    print(f"Uploading {total} bytes …")

    while offset < total:
        chunk   = image[offset: offset + CHUNK_SIZE]
        payload = {"data": chunk, "off": offset}
        if offset == 0:
            payload["image"] = 0
            payload["len"]   = total
            payload["sha"]   = bytes.fromhex(hash_hex)

        # First write may block while macOS prompts for the passkey.
        resp = await smp.request(OP_WRITE, GRP_IMAGE, ID_IMAGE_UPLOAD, payload,
                                  timeout=60.0 if offset == 0 else 30.0)
        rc, err = _check_rc(resp)
        if rc != 0:
            sys.exit(f"\nerror: upload failed at offset {offset}, rc={rc}"
                     f"{' (' + err + ')' if err else ''}\n       response: {resp!r}")

        offset = resp.get("off", offset + len(chunk))
        pct    = min(offset * 100 // total, 100)
        print(f"\r  {pct:3d}%  ({offset}/{total} B)", end="", flush=True)

    print()


async def image_state_dump(smp: SMPClient):
    """Read image state and print what the device sees in each slot."""
    print("Reading image state …")
    resp = await smp.request(OP_READ, GRP_IMAGE, ID_IMAGE_STATE, {})
    rc, err = _check_rc(resp)
    if rc != 0:
        print(f"  image-state-read failed: rc={rc} {err}")
        return
    images = resp.get("images", [])
    if not images:
        print("  (device reports no images)")
        return
    for img in images:
        h = img.get("hash", b"")
        hash_str = h.hex() if isinstance(h, (bytes, bytearray)) else "?"
        print(f"  slot={img.get('slot')} image={img.get('image', 0)} "
              f"version={img.get('version', '?')} hash={hash_str}")


async def image_test(smp: SMPClient, hash_hex: str):
    print("Marking image for test boot …")
    resp = await smp.request(OP_WRITE, GRP_IMAGE, ID_IMAGE_STATE,
                              {"hash": bytes.fromhex(hash_hex), "confirm": False})
    rc, err = _check_rc(resp)
    if rc != 0:
        # rc=33 IMAGE_SETTING_TEST_TO_ACTIVE_DENIED: img_mgmt_find_by_hash
        # matched the active slot, meaning the uploaded image is identical to
        # what's already running — there's nothing to "test".
        if rc == 33:
            sys.exit("error: the uploaded image is identical to the currently-"
                     "running image\n       (both slots have the same hash).  "
                     "Make any source change and\n       rebuild before re-running BLE DFU.")
        await image_state_dump(smp)
        sys.exit(f"error: image-test failed, rc={rc}"
                 f"{' (' + err + ')' if err else ''}")


async def image_confirm(smp: SMPClient):
    print("Confirming currently-running image …")
    resp = await smp.request(OP_WRITE, GRP_IMAGE, ID_IMAGE_STATE, {"confirm": True})
    rc, err = _check_rc(resp)
    if rc != 0:
        sys.exit(f"error: image-confirm failed, rc={rc}"
                 f"{' (' + err + ')' if err else ''}")
    print("Done. Image confirmed.")


async def reset(smp: SMPClient):
    print("Sending reset …")
    try:
        await asyncio.wait_for(
            smp.request(OP_WRITE, GRP_OS, ID_OS_RESET, {}), timeout=5.0)
    except Exception:
        pass  # device resets; response may not arrive


# ── Main ───────────────────────────────────────────────────────────────────

async def run(args):
    image_path = os.path.join(args.build_dir, "flexitMC3", "zephyr", "zephyr.signed.bin")

    if not args.confirm:
        if not os.path.isfile(image_path):
            sys.exit(f"error: signed image not found at {image_path}\n"
                     f"       Run a build first.")
        hash_hex = image_hash(image_path)
        image    = open(image_path, "rb").read()
        print(f"Image : {image_path}  ({len(image)} B)")
        print(f"Hash  : {hash_hex}")

    # 1. Try the cached address from a previous successful run first — this
    #    avoids macOS's name-scan cache which can stay stale for many seconds
    #    after a disconnect.
    device = None
    cached = _load_cached_address()
    if cached:
        print(f"Trying cached address {cached} (up to 5 s) …")
        device = await BleakScanner.find_device_by_address(cached, timeout=5)
        if not device:
            print("  cached address not seen — falling back to name scan.")

    # 2. Name scan with retries.
    if not device:
        attempts = 3
        for attempt in range(1, attempts + 1):
            if attempt == 1:
                print(f"Scanning for '{args.name}' (up to {args.scan_timeout:.0f} s) …")
            else:
                await asyncio.sleep(2)
                print(f"  not found yet — retrying ({attempt}/{attempts}) …")
            device = await BleakScanner.find_device_by_filter(
                lambda d, _: d.name == args.name, timeout=args.scan_timeout)
            if device:
                break
        if not device:
            sys.exit(
                f"error: '{args.name}' not found after {attempts} scans.\n"
                f"       If the device just disconnected from another BLE client,\n"
                f"       macOS may still be caching stale state — wait ~10 seconds\n"
                f"       and retry, or clear the entry under System Settings →\n"
                f"       Bluetooth → (i) next to '{args.name}' → Forget.")

    _save_cached_address(device.address)

    print(f"Found  {device.name}  [{device.address}] — connecting …")
    async with BleakClient(device) as client:
        print("Connected.")
        smp = SMPClient(client)
        await smp.start()

        if args.confirm:
            await image_confirm(smp)
        else:
            await upload(smp, image, hash_hex)
            await image_test(smp, hash_hex)
            await reset(smp)
            print(
                "\nDone. The device is rebooting — MCUboot swaps the image.\n"
                "If it boots correctly and you want to keep it, confirm with:\n"
                "  python3 tools/ble_dfu_flash.py --confirm"
            )

        await smp.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("build_dir", nargs="?", default="build",
                   help="Sysbuild output directory (default: build)")
    p.add_argument("--name", default=DEVICE_NAME,
                   help=f"BLE device name to scan for (default: {DEVICE_NAME})")
    p.add_argument("--scan-timeout", type=float, default=15.0,
                   help="Scan timeout in seconds (default: 15)")
    p.add_argument("--confirm", action="store_true",
                   help="Confirm the currently-running image (don't flash)")
    args = p.parse_args()

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nAborted.")


if __name__ == "__main__":
    main()
