#!/usr/bin/env python3
"""
mock_orion_ble_ota.py — Mock Orion BLE firmware-update sender for Ori.

The BLE sibling of the retired mock_orion_ota.py: same job (push a .bin to a
real Ori and watch it install), same scenario-menu shape, but over the Ori Sync
Service's two firmware-update characteristics instead of a USB CDC port. Ori
has no reachable data port any more — BLE is the only update path (ota.md).

Requirements:
    pip install bleak cbor2

Usage:
    python tools/mock_orion_ble_ota.py                     # interactive menu
    python tools/mock_orion_ble_ota.py --scenario 1        # one scenario, then exit
    python tools/mock_orion_ble_ota.py --image path/to/firmware.bin
    python tools/mock_orion_ble_ota.py --address AA:BB:CC:DD:EE:FF

Defaults to firmware/.pio/build/ori/firmware.bin relative to the repo root.

Pairing note:
    These characteristics are MITM-encrypted and Orion-only, so the sending PC
    must already be the bonded Orion peer — pair with mock_orion_ble.py first
    (or with the real Orion app). An unbonded central gets its writes rejected
    with INSUFFICIENT_AUTHENTICATION; a bonded-but-not-Orion peer is refused by
    Ori's own writer check. That bond IS the authority to overwrite the app
    partition now that physical port access no longer stands in for it.

Scenarios (all but 1 are deliberate failures — each should leave Ori on its
"Update failed" screen with a specific reason, then recover):
    1  Successful update
    2  Link drop mid-stream          -> FAILED link_lost  (or ble_timeout)
    3  Wrong version claim           -> FAILED version_mismatch
    4  Image too large               -> REJECT too_large
    5  Corrupted image (bad sha)     -> FAILED hash_mismatch
    6  Truncated transfer            -> FAILED truncated
    7  Oversized stream              -> FAILED size_overflow
    8  Double BEGIN                  -> REJECT busy
    9  Malformed BEGIN               -> REJECT missing_fields
    10 Dropped fragment (resync)     -> recovers via RESUME, then succeeds
    11 Abort mid-stream              -> FAILED aborted
    12 Monitor only (no transfer)

Scenario 10 is the one with no USB-era equivalent: it exercises the
offset-rewind path that replaces "restart the whole image from zero", which is
the part of this protocol that only matters because the transport can now drop
a packet without dropping the link.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import struct
import sys
import time
from pathlib import Path
from typing import Optional

try:
    import cbor2
except ImportError:
    sys.exit("Missing dependency: pip install cbor2")

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("Missing dependency: pip install bleak")


# ── UUIDs ────────────────────────────────────────────────────────────────────
# Base: 6F726900-0000-4F72-9F00-000000000000; bytes 4-5 carry the offset.
# Kept byte-identical to mock_orion_ble.py's block so the two agree by eye.

_BASE = "6F726900-{:04X}-4F72-9F00-000000000000"

def _uuid(offset: int) -> str:
    return _BASE.format(offset)

SVC_ORI_SYNC    = _uuid(0x0000)
UUID_DIS_FW_REV = "00002a26-0000-1000-8000-00805f9b34fb"
UUID_FW_CTRL    = _uuid(0x0014)   # Read, Write (response), Notify — encrypted
UUID_FW_DATA    = _uuid(0x0015)   # Write NR + Write               — encrypted

# ── Transfer tuning ──────────────────────────────────────────────────────────
# Ori requests ATT_MTU 247 on connect (ble-protocol.md §5). Each data frame is
# [offset uint32 LE][payload], so the payload budget is mtu - 3 (ATT header)
# - 4 (our offset header). bleak exposes the negotiated MTU per-connection;
# these are the fallback/cap.
ATT_OVERHEAD   = 3
OFFSET_HEADER  = 4
MAX_PAYLOAD    = 240
FALLBACK_MTU   = 23

# Windowed checkpoint (ota.md's sender guide). Frames stream Write-No-Response
# for throughput; every WINDOW-th frame — and the last — goes out as a
# Write-with-response, whose ATT ack proves the whole preceding burst landed and
# blocks us until Ori has drained it. Without this the controller happily
# accepts more than NimBLE can process and fragments get dropped.
WINDOW_FRAMES  = 24

BEGIN_TIMEOUT  = 15.0    # BEGIN -> READY
END_TIMEOUT    = 60.0    # END -> VALIDATED (hash pass + 3.5 s linger + commit)
STALL_TIMEOUT  = 20.0    # no status frame at all while streaming

DEFAULT_IMAGE = Path(__file__).resolve().parent.parent / "firmware" / ".pio" / "build" / "ori" / "firmware.bin"


# ── Status-frame plumbing ────────────────────────────────────────────────────

class FwLink:
    """Wraps a connected BleakClient with the firmware-update conversation.

    Every Ori -> Orion frame is a CBOR map on the control characteristic:
        { "o": op } plus at most one of "r" (reason), "v" (version), "b" (bytes)
    """

    def __init__(self, client: BleakClient):
        self.client = client
        self.queue: asyncio.Queue = asyncio.Queue()
        self.acked_bytes = 0        # last PROGRESS
        self.resume_to: Optional[int] = None  # last RESUME offset, consumed by the sender
        self.last_status = 0.0
        self.mtu = FALLBACK_MTU

    # -- notify handling ------------------------------------------------------

    def _on_ctrl(self, _sender, data: bytearray) -> None:
        self.last_status = time.monotonic()
        try:
            msg = cbor2.loads(bytes(data))
        except Exception as exc:
            print(f"  [ctrl] undecodable frame ({exc}): {bytes(data).hex()}")
            return
        op = msg.get("o", "?")
        if op == "PROGRESS":
            self.acked_bytes = int(msg.get("b", 0))
            return                      # too chatty to queue; the sender polls it
        if op == "RESUME":
            self.resume_to = int(msg.get("b", 0))
            print(f"  [ctrl] RESUME -> rewind to offset {self.resume_to}")
            return
        self.queue.put_nowait(msg)
        detail = msg.get("r") or msg.get("v") or ""
        print(f"  [ctrl] {op}{(' ' + str(detail)) if detail else ''}")

    async def start(self) -> None:
        await self.client.start_notify(UUID_FW_CTRL, self._on_ctrl)
        try:
            self.mtu = self.client.mtu_size or FALLBACK_MTU
        except Exception:
            self.mtu = FALLBACK_MTU
        print(f"  negotiated ATT MTU: {self.mtu}")

    def payload_size(self) -> int:
        n = self.mtu - ATT_OVERHEAD - OFFSET_HEADER
        return max(1, min(n, MAX_PAYLOAD))

    # -- control writes -------------------------------------------------------

    async def send_ctrl(self, msg: dict) -> None:
        await self.client.write_gatt_char(UUID_FW_CTRL, cbor2.dumps(msg), response=True)

    async def begin(self, version: str, total: int, digest: bytes) -> None:
        print(f"  -> BEGIN v={version} size={total} sha={digest.hex()[:16]}...")
        await self.send_ctrl({"o": "BEGIN", "v": version, "n": total, "h": digest})

    async def end(self) -> None:
        print("  -> END")
        await self.send_ctrl({"o": "END"})

    async def abort(self) -> None:
        print("  -> ABORT")
        await self.send_ctrl({"o": "ABORT"})

    async def send_frame(self, offset: int, payload: bytes, response: bool) -> None:
        frame = struct.pack("<I", offset) + payload
        await self.client.write_gatt_char(UUID_FW_DATA, frame, response=response)

    # -- waiting --------------------------------------------------------------

    async def expect(self, *ops: str, timeout: float) -> Optional[dict]:
        """Wait for one of `ops`. Returns the frame, or None on timeout.

        Any other terminal frame (REJECT/FAILED) is returned too — callers check
        the op rather than assuming success, because on this protocol a failure
        is a normal, expected outcome for most of the scenarios below.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print(f"  [timeout] waited {timeout:.0f}s for {'/'.join(ops)}")
                return None
            try:
                msg = await asyncio.wait_for(self.queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                print(f"  [timeout] waited {timeout:.0f}s for {'/'.join(ops)}")
                return None
            if msg.get("o") in ops or msg.get("o") in ("REJECT", "FAILED"):
                return msg


# ── The streaming core ───────────────────────────────────────────────────────

async def stream_image(link: FwLink, image: bytes, *,
                       stop_after: Optional[int] = None,
                       drop_frame_at: Optional[int] = None,
                       extra_tail: int = 0,
                       progress: bool = True) -> bool:
    """Push `image` over the data characteristic.

    stop_after     — stop once this many bytes have been sent (truncation tests)
    drop_frame_at  — skip exactly one frame at this frame index, once, then
                     honour the RESUME that comes back (resync test)
    extra_tail     — send this many bytes beyond total_size (overflow test)

    Returns False if Ori reported a terminal failure mid-stream.
    """
    total = len(image) + extra_tail
    size = link.payload_size()
    offset = 0
    frame_index = 0
    dropped_once = False
    started = time.monotonic()
    link.last_status = started
    last_print = 0.0

    while offset < total:
        # Ori asked us to rewind (a fragment was lost). Its staging buffer is
        # contiguous, so the fix is simply to resume from the byte it wants.
        if link.resume_to is not None:
            offset = link.resume_to
            link.resume_to = None

        chunk_end = min(offset + size, total)
        if offset < len(image):
            payload = image[offset:min(chunk_end, len(image))]
            if chunk_end > len(image):                    # overflow tail
                payload += b"\xAA" * (chunk_end - len(image))
        else:
            payload = b"\xAA" * (chunk_end - offset)

        if stop_after is not None and offset >= stop_after:
            print(f"  [scenario] stopping stream at {offset}/{len(image)} bytes")
            return True

        if drop_frame_at is not None and frame_index == drop_frame_at and not dropped_once:
            dropped_once = True
            print(f"  [scenario] dropping frame {frame_index} (offset {offset}) on purpose")
            offset = chunk_end
            frame_index += 1
            continue

        # Windowed checkpoint: the ack on this one covers everything since the
        # last checkpoint. Also the final frame, so END can't overtake the data.
        checkpoint = (frame_index % WINDOW_FRAMES == WINDOW_FRAMES - 1) or (chunk_end >= total)
        await link.send_frame(offset, payload, response=checkpoint)

        offset = chunk_end
        frame_index += 1

        # A terminal frame can arrive mid-stream (size_overflow, or the user
        # walking out of range). Drain it without blocking.
        if not link.queue.empty():
            msg = link.queue.get_nowait()
            print(f"  [ctrl] {msg.get('o')} {msg.get('r', '')} (mid-stream)")
            if msg.get("o") in ("FAILED", "REJECT"):
                return False

        if progress and time.monotonic() - last_print > 1.0:
            last_print = time.monotonic()
            elapsed = max(0.001, last_print - started)
            pct = 100.0 * offset / total
            rate = offset / elapsed / 1024.0
            print(f"    {pct:5.1f}%  {offset}/{total} B   {rate:6.1f} KB/s   "
                  f"acked {link.acked_bytes}", end="\r", flush=True)

        if time.monotonic() - link.last_status > STALL_TIMEOUT:
            print(f"\n  [stall] no status frame for {STALL_TIMEOUT:.0f}s — giving up")
            return False

    if progress:
        elapsed = max(0.001, time.monotonic() - started)
        print(f"    100.0%  {total}/{total} B   "
              f"{total / elapsed / 1024.0:6.1f} KB/s   (took {elapsed:.1f}s)      ")
    return True


# ── Scenarios ────────────────────────────────────────────────────────────────

async def scenario_success(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    if not await stream_image(link, image):
        return False
    await link.end()
    msg = await link.expect("VALIDATED", timeout=END_TIMEOUT)
    if msg and msg["o"] == "VALIDATED":
        print(f"  OK: Ori accepted {msg.get('v')} - installing, then rebooting.")
        print("    The link drops during the flash commit; that is expected.")
        print("    Reconnect afterwards and read the Firmware Revision String to confirm.")
        return True
    return False


async def scenario_link_drop(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    await stream_image(link, image, stop_after=len(image) // 3)
    print("  [scenario] disconnecting mid-transfer — expect FAILED link_lost on Ori")
    await link.client.disconnect()
    return True


async def scenario_wrong_version(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    bogus = "9.9.9" if version != "9.9.9" else "9.9.8"
    print(f"  [scenario] claiming {bogus} for a binary stamped {version}")
    await link.begin(bogus, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    if not await stream_image(link, image):
        return False
    await link.end()
    msg = await link.expect("FAILED", timeout=END_TIMEOUT)
    return bool(msg and msg.get("r") == "version_mismatch")


async def scenario_too_large(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    huge = 4 * 1024 * 1024          # > the 3 MB OTA slot
    print(f"  [scenario] declaring {huge} bytes (slot is 3 MB)")
    await link.begin(version, huge, digest)
    msg = await link.expect("REJECT", timeout=BEGIN_TIMEOUT)
    return bool(msg and msg.get("r") == "too_large")


async def scenario_corrupted(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    bad = bytes(digest[:-1]) + bytes([digest[-1] ^ 0xFF])
    print("  [scenario] declaring a sha256 with its last byte flipped")
    await link.begin(version, len(image), bad)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    if not await stream_image(link, image):
        return False
    await link.end()
    msg = await link.expect("FAILED", timeout=END_TIMEOUT)
    return bool(msg and msg.get("r") == "hash_mismatch")


async def scenario_truncated(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    print("  [scenario] sending END after ~half the image")
    await stream_image(link, image, stop_after=len(image) // 2)
    await link.end()
    msg = await link.expect("FAILED", timeout=END_TIMEOUT)
    return bool(msg and msg.get("r") == "truncated")


async def scenario_oversized(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    print("  [scenario] streaming 4 KB more than the declared size")
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    await stream_image(link, image, extra_tail=4096)
    msg = await link.expect("FAILED", timeout=END_TIMEOUT)
    return bool(msg and msg.get("r") == "size_overflow")


async def scenario_double_begin(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    print("  [scenario] second BEGIN while the first is live")
    await link.begin(version, len(image), digest)
    msg = await link.expect("REJECT", timeout=BEGIN_TIMEOUT)
    ok = bool(msg and msg.get("r") == "busy")
    await link.abort()                      # leave Ori clean for the next run
    await link.expect("FAILED", timeout=10.0)
    return ok


async def scenario_malformed_begin(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    print("  [scenario] BEGIN with no size/sha")
    await link.send_ctrl({"o": "BEGIN", "v": version})
    msg = await link.expect("REJECT", timeout=BEGIN_TIMEOUT)
    return bool(msg and msg.get("r") == "missing_fields")


async def scenario_dropped_fragment(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    print("  [scenario] skipping one frame ~10% in; Ori should answer RESUME "
          "and the transfer should still complete")
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    frames_total = max(1, len(image) // link.payload_size())
    if not await stream_image(link, image, drop_frame_at=frames_total // 10):
        return False
    await link.end()
    msg = await link.expect("VALIDATED", timeout=END_TIMEOUT)
    if msg and msg["o"] == "VALIDATED":
        print(f"  OK: recovered from the dropped frame and installed {msg.get('v')}.")
        return True
    return False


async def scenario_abort(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    await link.begin(version, len(image), digest)
    if not (msg := await link.expect("READY", timeout=BEGIN_TIMEOUT)) or msg["o"] != "READY":
        return False
    await stream_image(link, image, stop_after=len(image) // 4)
    await link.abort()
    msg = await link.expect("FAILED", timeout=15.0)
    return bool(msg and msg.get("r") == "aborted")


async def scenario_monitor(link: FwLink, image: bytes, digest: bytes, version: str) -> bool:
    print("  Listening on the control characteristic for 30 s (Ctrl-C to stop).")
    print("  Nothing is sent — use this to watch a real Orion-driven update.")
    try:
        await asyncio.sleep(30.0)
    except asyncio.CancelledError:
        pass
    return True


SCENARIOS = [
    ("1",  "Successful update",         scenario_success),
    ("2",  "Link drop mid-stream",      scenario_link_drop),
    ("3",  "Wrong version claim",       scenario_wrong_version),
    ("4",  "Image too large",           scenario_too_large),
    ("5",  "Corrupted image (bad sha)", scenario_corrupted),
    ("6",  "Truncated transfer",        scenario_truncated),
    ("7",  "Oversized stream",          scenario_oversized),
    ("8",  "Double BEGIN (busy)",       scenario_double_begin),
    ("9",  "Malformed BEGIN",           scenario_malformed_begin),
    ("10", "Dropped fragment (resync)", scenario_dropped_fragment),
    ("11", "Abort mid-stream",          scenario_abort),
    ("12", "Monitor only",              scenario_monitor),
]
SCENARIO_MAP = {key: fn for key, _, fn in SCENARIOS}


# ── Image handling ───────────────────────────────────────────────────────────

def extract_version(image: bytes) -> Optional[str]:
    """Read the version stamped inside the image ("OriFwVer=<ver>" marker).

    This is what BEGIN must claim: Ori re-reads the same marker out of the
    staged image and fails with version_mismatch if the two disagree. Never
    hardcode or guess it (ota.md step 0).
    """
    i = image.find(b"OriFwVer=")
    if i < 0:
        return None
    start = i + len(b"OriFwVer=")
    end = image.find(b"\x00", start)
    if end < 0:
        return None
    try:
        return image[start:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def load_image(path: Path) -> tuple[bytes, bytes, str]:
    if not path.is_file():
        sys.exit(f"Firmware image not found: {path}\n"
                 f"Build it first (pio run) or pass --image.")
    data = path.read_bytes()
    if not data:
        sys.exit(f"Firmware image is empty: {path}")
    if data[0] != 0xE9:
        print(f"  [warn] first byte is 0x{data[0]:02X}, not 0xE9 — "
              f"this may not be an ESP32 app image (Ori will say bad_image).")
    version = extract_version(data)
    if version is None:
        sys.exit("No OriFwVer= marker in the image — not a valid Ori build.")
    return data, hashlib.sha256(data).digest(), version


# ── Connection ───────────────────────────────────────────────────────────────

async def find_ori(address: Optional[str]) -> Optional[str]:
    if address:
        return address
    print("Scanning for Ori devices (10 s)...")
    found = await BleakScanner.discover(timeout=10.0, service_uuids=[SVC_ORI_SYNC])
    for d in found:
        if d.name == "Ori":
            print(f"  Found: {d.name}  [{d.address}]")
            return d.address
    if found:
        print("  No 'Ori' device found. Nearby devices:")
        for d in found:
            print(f"    {d.name or '(unknown)'}  [{d.address}]")
    return None


async def run_scenario(address: str, key: str, image: bytes, digest: bytes,
                       version: str) -> None:
    title = next(t for k, t, _ in SCENARIOS if k == key)
    fn = SCENARIO_MAP[key]
    print(f"\n=== [{key}] {title} ===")
    async with BleakClient(address) as client:
        print(f"  connected to {address}")
        try:
            raw = await client.read_gatt_char(UUID_DIS_FW_REV)
            print(f"  Ori is currently running: {raw.decode('utf-8', 'replace')}")
        except Exception as exc:
            print(f"  [warn] couldn't read the Firmware Revision String: {exc}")
        link = FwLink(client)
        await link.start()
        try:
            ok = await fn(link, image, digest, version)
        except Exception as exc:
            print(f"  [error] scenario raised: {exc!r}")
            ok = False
        print(f"  -> {'PASS' if ok else 'FAIL'}")
        # A successful install reboots Ori; disconnecting a dead link throws.
        try:
            if client.is_connected:
                await client.stop_notify(UUID_FW_CTRL)
        except Exception:
            pass


async def main_async(args) -> None:
    image, digest, version = load_image(Path(args.image))
    print(f"Image:   {args.image}")
    print(f"  size:  {len(image)} bytes")
    print(f"  sha256:{digest.hex()}")
    print(f"  OriFwVer marker: {version}")

    address = await find_ori(args.address)
    if not address:
        sys.exit("No Ori found. Power it on, or pass --address.")

    if args.scenario:
        await run_scenario(address, args.scenario, image, digest, version)
        return

    while True:
        print("\nScenarios:")
        for key, title, _ in SCENARIOS:
            print(f"  {key:>2}  {title}")
        print("   q  quit")
        try:
            choice = input("Choose: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if choice.lower() in ("q", "quit", "exit"):
            return
        if choice not in SCENARIO_MAP:
            print("  Unknown choice.")
            continue
        try:
            await run_scenario(address, choice, image, digest, version)
        except KeyboardInterrupt:
            print("\n  [interrupted] scenario aborted.")
        except Exception as exc:
            print(f"  [error] {exc!r}")
        print("\n  Ori may be rebooting or showing an error screen — "
              "give it a few seconds before the next scenario.")


def main() -> None:
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    parser = argparse.ArgumentParser(
        description="Mock Orion - BLE firmware-update scenario tester for Ori")
    parser.add_argument("--image", default=str(DEFAULT_IMAGE),
                        help="firmware .bin to send (default: the ori PlatformIO build output)")
    parser.add_argument("--address", metavar="ADDR",
                        help="Ori's BLE address (skips the scan)")
    parser.add_argument("--scenario", metavar="N", choices=SCENARIO_MAP,
                        help="run one scenario non-interactively, then exit")
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\nInterrupted.")


if __name__ == "__main__":
    main()
