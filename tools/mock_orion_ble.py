#!/usr/bin/env python3
"""
mock_orion_ble.py — Mock Orion BLE central for testing Ori's setup flow.

Connects to an Ori device and drops into an interactive command loop so you
can run a setup sync or issue a factory reset whenever you like — without
restarting the script.

Requirements:
    pip install bleak cbor2 Pillow

Usage:
    python tools/mock_orion_ble.py
    python tools/mock_orion_ble.py --address AA:BB:CC:DD:EE:FF

Interactive commands (shown on connect):
    s  — first-time setup sync   (§6.1)
    r  — reconnect delta sync    (§6.2)
    f  — factory reset           (§7.2)
    q  — quit

Pairing note:
    BLE bonding (LE Secure Connections, Passkey Entry) is handled by the OS.
    When the encrypted writes start, Windows will show a pairing dialog.
    Confirm the 6-digit code shown on Ori's screen. The bond is cached after
    first pair, so subsequent runs connect silently.

Factory reset note:
    After sending a factory reset, Ori wipes its bond and reboots. You must
    also remove it from Windows Bluetooth settings before re-pairing:
    Settings → Bluetooth → find Ori-XX-XX → Remove device.
    Then press 's' to run the setup sync again.
"""

import asyncio
import argparse
import hashlib
import io
import logging
import os
import struct
import time
from datetime import datetime, timezone
from typing import Optional

import cbor2
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

try:
    from PIL import Image as _PILImage
    _PIL_AVAILABLE = True
except ImportError:
    _PILImage = None
    _PIL_AVAILABLE = False

# Paths to source images resolved relative to this script.
_SCRIPT_DIR        = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT         = os.path.dirname(_SCRIPT_DIR)
_PROFILE_PHOTO_SRC = os.path.join(_REPO_ROOT, "firmware", "img", "profile_photo", "profile_photo.png")
_PTO_PHOTO_SRC     = os.path.join(_REPO_ROOT, "firmware", "img", "pto_photo",     "pto_photo.jpg")

# ── UUIDs ────────────────────────────────────────────────────────────────────
# Base: 6F726900-0000-4F72-9F00-000000000000
# Each char UUID replaces bytes 4-5 (second group) with its offset.

_BASE = "6F726900-{:04X}-4F72-9F00-000000000000"

def _uuid(offset: int) -> str:
    return _BASE.format(offset)

SVC_ORI_SYNC      = _uuid(0x0000)
UUID_PROTO_VER    = _uuid(0x0001)   # Read                  — unencrypted
UUID_DEV_STATUS   = _uuid(0x0002)   # Read, Notify          — unencrypted
UUID_TIME_SYNC    = _uuid(0x0003)   # Write (response)      — encrypted
UUID_PROFILE      = _uuid(0x0004)   # Write (response)      — encrypted
UUID_PHOTO        = _uuid(0x0005)   # Write chunked         — encrypted
UUID_MEETINGS     = _uuid(0x0006)   # Write chunked         — encrypted
UUID_PTO          = _uuid(0x0007)   # Write chunked         — encrypted
UUID_SYNC_CTRL    = _uuid(0x0008)   # Write, Notify         — encrypted
UUID_FACTORY_RST  = _uuid(0x0009)   # Write (response)      — encrypted
UUID_MANIFEST     = _uuid(0x000A)   # Write, Notify         — encrypted

FACTORY_RESET_MAGIC = bytes([0xFA, 0xC7, 0x5E, 0x5E])

# ── Device Status values ─────────────────────────────────────────────────────

DS = {
    0x00: "SETUP_WAITING_PAIRING",
    0x01: "SETUP_BONDED_AWAITING_SYNC",
    0x02: "SETUP_SYNCING",
    0x03: "SETUP_SYNC_COMPLETE",
    0x10: "RUNTIME_READY",
    0x11: "RUNTIME_RECONNECTING",
    0x12: "RUNTIME_SYNCING",
}

# ── Chunking ─────────────────────────────────────────────────────────────────
# Frame: seq_num(u16 LE) | total_frags(u16 LE) | payload_len(u16 LE) | payload
# MTU 247 → 247 - 3 ATT - 6 header = 238 bytes payload per frame.

FRAG_SIZE = 238

def make_frames(payload: bytes) -> list[bytes]:
    if not payload:
        # One zero-length frame signals "no data" to the firmware.
        return [struct.pack("<HHH", 0, 1, 0)]
    chunks = [payload[i:i + FRAG_SIZE] for i in range(0, len(payload), FRAG_SIZE)]
    total  = len(chunks)
    return [struct.pack("<HHH", seq, total, len(c)) + c
            for seq, c in enumerate(chunks)]

# ── Photo builders ────────────────────────────────────────────────────────────

def _jpeg_max_quality(img, hard_cap: int) -> bytes:
    """Return the highest-quality JPEG of img that fits within hard_cap bytes."""
    for q in range(95, 9, -1):
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=q, optimize=True)
        data = buf.getvalue()
        if len(data) <= hard_cap:
            return data
    # quality=10 still exceeds cap — shouldn't happen at these dimensions + caps
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=10)
    return buf.getvalue()

def build_profile_photo_jpeg() -> bytes:
    """Center-crop and resize profile photo to 228×228, compress to ≤ 40 KB."""
    if not _PIL_AVAILABLE:
        print("  [warn] Pillow not installed — profile photo empty  (pip install Pillow)")
        return b""
    if not os.path.exists(_PROFILE_PHOTO_SRC):
        print(f"  [warn] Profile photo not found: {_PROFILE_PHOTO_SRC}")
        return b""
    img = _PILImage.open(_PROFILE_PHOTO_SRC).convert("RGB")
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2))
    img = img.resize((228, 228), _PILImage.LANCZOS)
    data = _jpeg_max_quality(img, hard_cap=200 * 1024)
    print(f"  [info] profile photo: 228×228 JPEG, {len(data):,} B")
    return data

def build_pto_photo_jpeg() -> bytes:
    """Crop PTO photo to 528:396 aspect ratio, resize to 528×396, compress to ≤ 64 KB."""
    if not _PIL_AVAILABLE:
        print("  [warn] Pillow not installed — PTO photo empty  (pip install Pillow)")
        return b""
    if not os.path.exists(_PTO_PHOTO_SRC):
        print(f"  [warn] PTO photo not found: {_PTO_PHOTO_SRC}")
        return b""
    img = _PILImage.open(_PTO_PHOTO_SRC).convert("RGB")
    w, h   = img.size
    tw, th = 528, 396
    if w / h > tw / th:                      # source too wide — crop sides
        nw = int(h * tw / th)
        img = img.crop(((w - nw) // 2, 0, (w + nw) // 2, h))
    else:                                    # source too tall — crop top/bottom
        nh = int(w * th / tw)
        img = img.crop((0, (h - nh) // 2, w, (h + nh) // 2))
    img  = img.resize((tw, th), _PILImage.LANCZOS)
    data = _jpeg_max_quality(img, hard_cap=512 * 1024)
    print(f"  [info] PTO photo: {tw}×{th} JPEG, {len(data):,} B")
    return data

# ── Mock data ─────────────────────────────────────────────────────────────────

def build_time_sync() -> bytes:
    now_utc = int(datetime.now(timezone.utc).timestamp())
    tx_ms   = int(time.monotonic() * 1000)
    return cbor2.dumps({"epoch_utc": now_utc, "tz": "America/Los_Angeles", "tx_ms": tx_ms})

def build_profile() -> bytes:
    # All fields maximized to their wire byte caps (ble-protocol.md §10).
    return cbor2.dumps({
        "name":  ("Stress-Test Name Padding " * 3)[:64],
        "title": ("Stress-Test Title Padding " * 3)[:64],
        "email": ("stress.test.email@very-long-corporate-domain.engineering.example.com" * 2)[:128],
        "phone": ("+1-555-867-5309-ext-00001234" * 2)[:32],
    })

def build_meetings() -> bytes:
    # 32 meetings (protocol cap) with near-maximum field lengths.
    # Per-meeting estimate ~122 B × 32 + 19 B wrapper ≈ 3,923 B — under the
    # firmware boot buffer cap of 4,096 B (main.cpp: static uint8_t meet_buf[4096]).
    now      = int(datetime.now(timezone.utc).timestamp())
    midnight = now - (now % 86400)
    items = []
    for i in range(32):
        start = midnight + (8 + i // 2) * 3600 + (i % 2) * 1800   # 08:00–23:30
        items.append({
            "id":    f"ev{i:02d}",
            "start": start,
            "end":   start + 3600,
            "title": (f"Stress Meeting {i:02d}: " + "X" * 40)[:40],
            "loc":   (f"Room-{i:02d}/Video "      + "X" * 20)[:20],
            "org":   (f"Organizer{i:02d} "         + "X" * 15)[:15],
        })
    data = cbor2.dumps({"date": midnight, "items": items})
    if len(data) >= 4096:
        raise ValueError(
            f"meetings CBOR too large: {len(data)} B (firmware boot buffer limit 4096 B)")
    return data

def build_pto(image: bytes = b"") -> bytes:
    # destination maximized to its 128-byte wire cap (ble-protocol.md §10).
    now         = int(datetime.now(timezone.utc).timestamp())
    week        = 7 * 86400
    destination = ("Tokyo, Japan — Cherry Blossom Season Adventure " * 3)[:128]
    return cbor2.dumps({
        "start":       now + week,
        "end":         now + 2 * week,
        "destination": destination,
        "image":       image,
    })

def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()

# ── Session ───────────────────────────────────────────────────────────────────

class MockOrion:
    def __init__(self, client: BleakClient):
        self.client          = client
        self._status         = asyncio.Event()
        self._manifest_reply = asyncio.Event()
        self.last_status: Optional[int] = None
        self.needed: list[str] = []
        self.disconnected     = False

    # ── notification handlers ──

    def _on_dev_status(self, _: BleakGATTCharacteristic, data: bytearray):
        val  = data[0]
        name = DS.get(val, f"0x{val:02X}")
        print(f"\n  [notify] DeviceStatus → {name}")
        self.last_status = val
        self._status.set()

    def _on_sync_ctrl(self, _: BleakGATTCharacteristic, data: bytearray):
        try:
            msg = cbor2.loads(bytes(data))
            print(f"\n  [notify] SyncControl ← {msg}")
        except Exception:
            print(f"\n  [notify] SyncControl ← raw {data.hex()}")

    def _on_manifest(self, _: BleakGATTCharacteristic, data: bytearray):
        try:
            msg = cbor2.loads(bytes(data))
            self.needed = msg.get("needed", [])
            print(f"\n  [notify] SyncManifest ← needed={self.needed}")
        except Exception:
            print(f"\n  [notify] SyncManifest ← raw {data.hex()}")
        self._manifest_reply.set()

    # ── helpers ──

    async def wait_status(self, target: int, timeout: float = 30.0) -> bool:
        deadline = time.monotonic() + timeout
        while True:
            if self.last_status == target:
                return True
            self._status.clear()
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            try:
                await asyncio.wait_for(self._status.wait(), timeout=left)
            except asyncio.TimeoutError:
                return False

    async def write(self, uuid: str, data: bytes, label: str):
        await self.client.write_gatt_char(uuid, bytearray(data), response=True)
        print(f"  [write]  {label} ({len(data)} B)")

    async def write_chunked(self, uuid: str, payload: bytes, label: str):
        frames = make_frames(payload)
        suffix = "empty" if not payload else f"{len(payload):,} B → {len(frames)} frame(s)"
        print(f"  [chunk]  {label}: {suffix}")
        for frame in frames:
            await self.client.write_gatt_char(uuid, bytearray(frame), response=True)

    async def subscribe(self):
        await self.client.start_notify(UUID_DEV_STATUS, self._on_dev_status)
        await self.client.start_notify(UUID_SYNC_CTRL,  self._on_sync_ctrl)
        await self.client.start_notify(UUID_MANIFEST,   self._on_manifest)
        raw = await self.client.read_gatt_char(UUID_DEV_STATUS)
        self._on_dev_status(None, raw)  # seed current value

    def _print_hashes(self, profile, photo, meetings, pto):
        print("── SHA-256 hashes (reference for next reconnect) ──")
        print(f"  profile:  {sha256(profile).hex()}")
        print(f"  photo:    {sha256(photo).hex()}")
        print(f"  meetings: {sha256(meetings).hex()}")
        print(f"  pto:      {sha256(pto).hex()}")

    # ── setup sync §6.1 ──────────────────────────────────────────────────────

    async def run_setup(self):
        print("\n═══ First-time setup sync ═══")
        print("   (Complete BLE pairing on Ori if prompted — confirm passkey)")

        if not await self.wait_status(0x01, timeout=60.0):
            print(f"  [error] Timed out waiting for SETUP_BONDED_AWAITING_SYNC")
            print(f"  Last status: {DS.get(self.last_status, '?')}")
            print("  Is Ori in setup mode and bonded?")
            return

        print("\n── Building payload ──")
        time_sync_bytes = build_time_sync()
        profile_bytes   = build_profile()
        photo_bytes     = build_profile_photo_jpeg()
        meetings_bytes  = build_meetings()
        pto_image       = build_pto_photo_jpeg()
        pto_bytes       = build_pto(image=pto_image)

        total = (len(time_sync_bytes) + len(profile_bytes) + len(photo_bytes)
                 + len(meetings_bytes) + len(pto_bytes))

        print(f"  [info] byte breakdown:"
              f"  time_sync={len(time_sync_bytes)}"
              f"  profile={len(profile_bytes)}"
              f"  photo={len(photo_bytes):,}"
              f"  meetings={len(meetings_bytes)}"
              f"  pto={len(pto_bytes):,}"
              f"  → total={total:,}")

        print("\n── Writing sync data ──")
        seq = 1
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"op": "BEGIN", "seq": seq, "total": total}),
            f"SyncControl BEGIN seq={seq} total={total:,}")
        await self.write(UUID_TIME_SYNC,  time_sync_bytes,   "TimeSync")
        await self.write(UUID_PROFILE,    profile_bytes,     "ProfileInfo")
        await self.write_chunked(UUID_PHOTO,    photo_bytes,    "ProfilePhoto")
        await self.write_chunked(UUID_MEETINGS, meetings_bytes, "MeetingList")
        await self.write_chunked(UUID_PTO,      pto_bytes,      "PtoEntry")
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"op": "END", "seq": seq}), f"SyncControl END seq={seq}")

        print("\n── Waiting for SETUP_SYNC_COMPLETE ──")
        if await self.wait_status(0x03, timeout=30.0):
            print("  [ok] Sync complete — Ori should advance to Step 4.\n")
        else:
            print(f"  [warn] SETUP_SYNC_COMPLETE not received "
                  f"(last: {DS.get(self.last_status, '?')})\n")

        self._print_hashes(profile_bytes, photo_bytes, meetings_bytes, pto_bytes)

    # ── reconnect delta sync §6.2 ─────────────────────────────────────────────

    async def run_reconnect(self):
        print("\n═══ Reconnect delta sync ═══")

        if not await self.wait_status(0x11, timeout=30.0):
            print(f"  [warn] Expected RUNTIME_RECONNECTING, got "
                  f"{DS.get(self.last_status, '?')} — proceeding anyway")

        print("\n── Building payload ──")
        time_sync_bytes = build_time_sync()
        profile_bytes   = build_profile()
        photo_bytes     = build_profile_photo_jpeg()
        meetings_bytes  = build_meetings()
        pto_image       = build_pto_photo_jpeg()
        pto_bytes       = build_pto(image=pto_image)

        print("\n── Writing TimeSync (always) ──")
        await self.write(UUID_TIME_SYNC, time_sync_bytes, "TimeSync")

        print("\n── Writing Sync Manifest ──")
        self._manifest_reply.clear()
        await self.write(UUID_MANIFEST, cbor2.dumps({
            "profile_sha":  sha256(profile_bytes),
            "photo_sha":    sha256(photo_bytes),
            "meetings_sha": sha256(meetings_bytes),
            "pto_sha":      sha256(pto_bytes),
        }), "SyncManifest")

        print("── Waiting for Ori manifest reply ──")
        try:
            await asyncio.wait_for(self._manifest_reply.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            print("  [warn] No manifest reply — assuming all data needed")
            self.needed = ["profile", "photo", "meetings", "pto"]

        if not self.needed:
            print("  [ok] Nothing changed — skipping writes")
        else:
            item_bytes = {
                "profile":  profile_bytes,
                "photo":    photo_bytes,
                "meetings": meetings_bytes,
                "pto":      pto_bytes,
            }
            total = len(time_sync_bytes) + sum(
                len(item_bytes[k]) for k in self.needed if k in item_bytes)

            print(f"  [info] byte breakdown:  time_sync={len(time_sync_bytes)}"
                  + "".join(f"  {k}={len(item_bytes[k]):,}"
                             for k in self.needed if k in item_bytes)
                  + f"  → total={total:,}")

            seq = 2
            await self.write(UUID_SYNC_CTRL,
                cbor2.dumps({"op": "BEGIN", "seq": seq, "total": total}),
                f"SyncControl BEGIN seq={seq} total={total:,}")
            if "profile"  in self.needed:
                await self.write(UUID_PROFILE, profile_bytes, "ProfileInfo")
            if "photo"    in self.needed:
                await self.write_chunked(UUID_PHOTO, photo_bytes, "ProfilePhoto")
            if "meetings" in self.needed:
                await self.write_chunked(UUID_MEETINGS, meetings_bytes, "MeetingList")
            if "pto"      in self.needed:
                await self.write_chunked(UUID_PTO, pto_bytes, "PtoEntry")
            await self.write(UUID_SYNC_CTRL,
                cbor2.dumps({"op": "END", "seq": seq}), f"SyncControl END seq={seq}")

        print("\n── Waiting for RUNTIME_READY ──")
        if await self.wait_status(0x10, timeout=30.0):
            print("  [ok] Reconnect overlay dismissed — Ori is live.\n")
        else:
            print(f"  [warn] RUNTIME_READY not received "
                  f"(last: {DS.get(self.last_status, '?')})\n")

        self._print_hashes(profile_bytes, photo_bytes, meetings_bytes, pto_bytes)

    # ── factory reset §7.2 ───────────────────────────────────────────────────

    async def run_factory_reset(self):
        print("\n═══ Factory Reset ═══")
        loop = asyncio.get_event_loop()
        confirm = await loop.run_in_executor(
            None, input, "  Send factory reset to Ori? [y/N] ")
        if confirm.strip().lower() != "y":
            print("  Cancelled.")
            return

        try:
            await self.write(UUID_FACTORY_RST, FACTORY_RESET_MAGIC, "FactoryReset magic")
        except Exception as exc:
            # Ori may reboot before the write response arrives — that's fine.
            print(f"  [note] Write response not received ({exc}) — "
                  "Ori may have already rebooted.")

        print()
        print("  [ok] Factory reset sent. Ori is wiping NVS and rebooting.")
        print()
        print("  ── Next steps to re-run setup ──────────────────────────────")
        print("  1. Open Windows Settings → Bluetooth & devices")
        print("     Find 'Ori-XX-XX' → click '...' → Remove device")
        print("  2. Wait ~5 s for Ori to finish rebooting (Welcome screen)")
        print("  3. Press 's' here to run the setup sync")
        print("  ────────────────────────────────────────────────────────────")
        print()

        self.disconnected = True  # signal the command loop to reconnect

# ── Interactive command loop ──────────────────────────────────────────────────

MENU = """
Commands:
  s — setup sync      (first-time, §6.1)
  r — reconnect sync  (delta, §6.2)
  f — factory reset   (§7.2)
  q — quit
"""

async def command_loop(orion: MockOrion):
    loop = asyncio.get_event_loop()
    print(MENU)
    while not orion.disconnected:
        try:
            cmd = await loop.run_in_executor(None, input, "> ")
        except EOFError:
            break
        cmd = cmd.strip().lower()
        if not orion.client.is_connected:
            print("  [error] BLE link is down. Quit and reconnect.")
            break
        if cmd == "s":
            await orion.run_setup()
            print(MENU)
        elif cmd == "r":
            await orion.run_reconnect()
            print(MENU)
        elif cmd == "f":
            await orion.run_factory_reset()
            # Don't reprint menu — factory reset exits the loop.
        elif cmd == "q":
            print("  Goodbye.")
            break
        elif cmd:
            print("  Unknown command. Type s / r / f / q")

# ── Entry point ───────────────────────────────────────────────────────────────

async def find_ori(address: Optional[str]) -> Optional[str]:
    if address:
        return address
    print("Scanning for Ori devices (10 s)…")
    found = await BleakScanner.discover(timeout=10.0, service_uuids=[SVC_ORI_SYNC])
    for d in found:
        if d.name and d.name.startswith("Ori-"):
            print(f"  Found: {d.name}  [{d.address}]")
            return d.address
    if found:
        print("  No Ori-* device found. Nearby devices:")
        for d in found:
            print(f"    {d.name or '(unknown)'}  [{d.address}]")
    return None

def on_disconnect(_: BleakClient):
    print("\n  [disconnected] BLE link dropped (Ori may have rebooted).")

async def main(args):
    address = await find_ori(args.address)
    if not address:
        print("No Ori device found. Use --address to specify one directly.")
        return

    print(f"\nConnecting to {address}…")
    async with BleakClient(address, timeout=20.0,
                           disconnected_callback=on_disconnect) as client:
        print(f"Connected. MTU = {client.mtu_size}")

        print("Pairing… (confirm the 6-digit code shown on Ori's screen)")
        await client.pair()
        print("  [ok] Paired and bonded.")

        orion = MockOrion(client)
        await orion.subscribe()
        await command_loop(orion)

if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    parser = argparse.ArgumentParser(
        description="Mock Orion BLE — interactive Ori tester",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--address", metavar="AA:BB:CC:DD:EE:FF",
                        help="BLE address of Ori (skips scan)")
    asyncio.run(main(parser.parse_args()))
