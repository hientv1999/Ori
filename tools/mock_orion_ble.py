#!/usr/bin/env python3
"""
mock_orion_ble.py — Mock Orion BLE central for testing Ori's setup flow.

Connects to an Ori device and drops into an interactive command loop so you
can run a setup sync or issue a factory reset whenever you like — without
restarting the script.

Requirements:
    pip install bleak cbor2 Pillow
    pip install winsdk   # optional, Windows only — needed for media commands
                          # (reads the current Windows now-playing media session)
    pip install pycaw    # optional, Windows only — needed for volume control
                          # (IAudioEndpointVolume — bidirectional volume sync)

Usage:
    python tools/mock_orion_ble.py
    python tools/mock_orion_ble.py --address AA:BB:CC:DD:EE:FF
    python tools/mock_orion_ble.py --presence 1   # push BUSY on connect
    python tools/mock_orion_ble.py --weather 3    # push RAIN on connect
    python tools/mock_orion_ble.py --stress-meetings   # 32 max-length meetings instead of 6 realistic ones

Interactive commands (shown on connect):
    s  — sync now (hash-driven delta; also runs automatically on connect)
    p  — push presence (cycles AVAILABLE -> BUSY -> AWAY -> OFFLINE -> ...)
    c  — push shortcuts (cycles through SHORTCUT_COMBOS test sets, incl. an
         unknown-token combo to exercise the fallback-icon path)
    t  — resync time (manual TimeSync-only push, RAM-only, no blackout)
    m  — push media now (manual one-shot push of the current Windows
         now-playing session — title, artist, embedded album art)
    k  — push clock face (toggles DIGITAL <-> ANALOG, char 000E Device Settings)
    h  — push time format (toggles 24-HOUR <-> 12-HOUR, char 000E Device Settings)
    n  — push ANCS filter (cycles DISABLED -> CALL_ONLY -> IMPORTANT -> ALL -> ...)
    w  — push weather (cycles CLEAR -> PARTLY_CLOUDY -> CLOUDY -> RAIN ->
         THUNDERSTORM -> SNOW -> FOG -> ..., see WEATHER_CYCLE for the
         paired temperature each condition sends)
    r  — read Device Settings (clock_face + time_format + ancs_filter currently in Ori's NVS)
    f  — factory reset           (§7.2)
    q  — quit

Media (Media Metadata + Media Album Art, ble-protocol.md §12) also auto-pushes
in the background: a watcher task polls Windows' now-playing session
(GlobalSystemMediaTransportControlsSessionManager) every 2 s and pushes
whenever the track actually changes, mirroring how the real Orion app's
Windows bridge reacts to OS media-change notifications. 'm' is just for
forcing an immediate push without waiting for the next poll. Windows only;
requires winsdk — degrades to a one-time warning (and the watcher silently
does nothing) if it isn't installed.

Volume (Host Volume State, ble-protocol.md §12) is also bidirectional:
- On connect: the current Windows master volume is immediately pushed to Ori
  (HostVolumeState) so Ori's swipe bar starts at the correct level.
- Volume watcher: polls Windows master volume every ~1 s and pushes
  HostVolumeState to Ori whenever it changes (e.g. system volume slider,
  other apps). Windows only; requires pycaw — degrades silently if not
  installed.
- vol_set from Ori: when Ori sends KeyboardCommand{op:"vol_set", arg:N} after
  a vertical-swipe release, the mock sets Windows master volume to N% and
  writes HostVolumeState{N} back to Ori as confirmation. Ori ignores the
  write-back during its 800 ms swipe-override window (ble-protocol.md §12).

On connect the mock runs ONE unified sync: it sends Ori a manifest of every
section's hash (time/profile/photo/meetings/Time Off), Ori replies with the subset
that differs, and only those are sent — everything on a first pair, or just the
changed/dropped sections on a reconnect (e.g. meetings + time after a power
cycle). Time Sync is always sent.

Presence and weather are both pushed separately from the sync flow — neither has
a manifest or hash, and neither is returned by Device Settings read (Orion is the
sole source of truth for both). A fresh value of each is written right after
connecting (--presence default AVAILABLE, --weather default CLEAR); re-push
anytime with 'p' / 'w'. Weather's three fields ("w" condition + "d" temp_c +
"u" unit) are always sent together — Ori ignores a write with only some of the
three. Clock face
and ANCS filter ARE recovered via a Device Settings read on connect ('r' to
re-read manually), so 'k' starts from Ori's actual persisted value rather than a
script-side assumption.

Pairing note:
    BLE bonding (LE Secure Connections, Passkey Entry) is handled by the OS.
    When the encrypted writes start, Windows will show a pairing dialog.
    Confirm the 6-digit code shown on Ori's screen. The bond is cached after
    first pair, so subsequent runs connect silently.

Factory reset note:
    After a factory reset (sent remotely with 'f', or triggered locally on
    Ori via long-press), Ori wipes its bond and reboots. On disconnect this
    script automatically re-scans for Ori's advertising flag (ble-protocol.md
    §2/§7.1) and tells you whether it saw SETUP (factory reset — remove the
    stale Windows pairing first) or RUNTIME (just a reboot — bond intact).
    After removing the stale pairing in Windows Bluetooth settings
    (Settings → Bluetooth → find "Ori" → Remove device), re-run this
    script to pair again.
"""

import asyncio
import argparse
import ctypes
import hashlib
import io
import logging
import os
import struct
import time
from datetime import datetime, timezone, timedelta
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

# Windows now-playing media (ble-protocol.md §12 — GlobalSystemMediaTransport-
# ControlsSessionManager, the same OS API the real Orion app's Windows bridge
# uses). Windows-only, optional — only needed for the 'm' command.
try:
    from winsdk.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as _MediaManager,
    )
    from winsdk.windows.storage.streams import (
        Buffer as _WinBuffer,
        InputStreamOptions as _WinInputStreamOptions,
    )
    _WINRT_AVAILABLE = True
except ImportError:
    _WINRT_AVAILABLE = False

# Windows master-volume control (pycaw — wraps IAudioEndpointVolume).
# Windows-only, optional — only needed for volume sync. Degrades silently if
# pycaw isn't installed (pip install pycaw).
try:
    from pycaw.pycaw import AudioUtilities as _AudioUtils, IAudioEndpointVolume as _IAudioEPVol
    from ctypes import cast as _ctypes_cast, POINTER as _POINTER
    from comtypes import CLSCTX_ALL as _CLSCTX_ALL
    _PYCAW_AVAILABLE = True
except ImportError:
    _PYCAW_AVAILABLE = False

# Paths to source images resolved relative to this script.
_SCRIPT_DIR        = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT         = os.path.dirname(_SCRIPT_DIR)
_PROFILE_PHOTO_SRC = os.path.join(_REPO_ROOT, "firmware", "img", "profile_photo", "profile_photo.jfif")
_TIME_OFF_PHOTO_SRC = os.path.join(_REPO_ROOT, "firmware", "img", "time_off_photo", "time_off_photo.jpg")

# Forced JPEG quality for the profile + Time Off photos. None = pick the highest
# quality that fits the hard cap (default). Set via --photo-quality to send
# deliberately lower-quality (smaller) images — useful for exercising the
# firmware JPEG decoder and shortening chunked transfers during testing.
_FORCED_JPEG_QUALITY: Optional[int] = None

# False (default) = a believable 6-meeting day starting from right now, for
# eyeballing the meeting list screen. True (--stress-meetings) = 32
# max-length meetings, the protocol-cap boundary test this script originally
# always sent.
_STRESS_MEETINGS: bool = False

# ── UUIDs ────────────────────────────────────────────────────────────────────
# Base: 6F726900-0000-4F72-9F00-000000000000
# Each char UUID replaces bytes 4-5 (second group) with its offset.

_BASE = "6F726900-{:04X}-4F72-9F00-000000000000"

def _uuid(offset: int) -> str:
    return _BASE.format(offset)

SVC_ORI_SYNC      = _uuid(0x0000)
# Firmware version is read via the BLE SIG standard Device Information
# Service (0x180A) / Firmware Revision String characteristic (0x2A26),
# not a custom char in this service — see ble-protocol.md §3/§3.1/§9.
UUID_DIS_FW_REV   = "00002a26-0000-1000-8000-00805f9b34fb"
UUID_DEV_STATUS   = _uuid(0x0001)   # Read, Notify          — unencrypted
UUID_TIME_SYNC    = _uuid(0x0002)   # Write (response)      — encrypted
UUID_PROFILE      = _uuid(0x0003)   # Write (response)      — encrypted
UUID_PHOTO        = _uuid(0x0004)   # Write chunked         — encrypted
UUID_MEETINGS     = _uuid(0x0005)   # Write chunked         — encrypted
UUID_TIME_OFF     = _uuid(0x0006)   # Write chunked         — encrypted
UUID_SYNC_CTRL    = _uuid(0x0007)   # Write, Notify         — encrypted
UUID_FACTORY_RST  = _uuid(0x0008)   # Write (response)      — encrypted
UUID_MANIFEST     = _uuid(0x0009)   # Write, Notify         — encrypted
UUID_KEYBOARD_CMD = _uuid(0x000A)   # Notify only               — encrypted (Ori → Orion)
UUID_HOST_VOLUME  = _uuid(0x000B)   # Read, Write (response)   — encrypted
UUID_MEDIA_META   = _uuid(0x000C)   # Write (response), Notify — encrypted
UUID_ALBUM_ART    = _uuid(0x000D)   # Write NO RESPONSE only   — encrypted
UUID_DEV_SETTINGS = _uuid(0x000E)   # Write (response)      — encrypted
UUID_PHONE_STATUS = _uuid(0x000F)   # Read, Notify          — encrypted (Ori → Orion)
                                    # Merges: presence | shortcut slots | clock face | ANCS filter

FACTORY_RESET_MAGIC = bytes([0xFA, 0xC7, 0x5E, 0x5E])

# Device Settings field value names (ble-protocol.md §4 DeviceSettings schema).
PRESENCE_NAMES   = {0x00: "AVAILABLE", 0x01: "BUSY", 0x02: "AWAY", 0x03: "OFFLINE"}
CLOCK_FACE_NAMES = {0x00: "DIGITAL", 0x01: "ANALOG"}
TIME_FORMAT_NAMES = {0x00: "24-HOUR", 0x01: "12-HOUR"}
ANCS_FILTER_NAMES = {0x00: "DISABLED", 0x01: "CALL_ONLY", 0x02: "IMPORTANT", 0x03: "ALL"}
WEATHER_NAMES = {
    0x00: "CLEAR", 0x01: "PARTLY_CLOUDY", 0x02: "CLOUDY", 0x03: "RAIN",
    0x04: "THUNDERSTORM", 0x05: "SNOW", 0x06: "FOG",
}

# (condition, temp_c) pairs to cycle through with 'w'. Values default to a
# Celsius scale — this mock always declares unit=Celsius via push_weather()'s
# temp_unit=1; Ori renders whatever integer + unit it's told, never converts,
# see ble-protocol.md §4) and are deliberately spread across the width extremes
# of the temperature-text label ("-40°C".."140°C", cap in ble-protocol.md §10)
# rather than all being plausible-looking, so 'w' exercises: a normal 2-digit
# positive (Clear/Partly Cloudy/Thunderstorm), a 1-digit positive (Cloudy), a
# 1-digit negative (Rain), a 2-digit negative (Snow), and the min-cap boundary
# (Fog).
WEATHER_CYCLE = [
    (0x00, 26),    # Clear            — 2-digit positive
    (0x01, 20),    # Partly Cloudy    — 2-digit positive
    (0x02, 8),     # Cloudy           — 1-digit positive
    (0x03, -3),    # Rain             — 1-digit negative
    (0x04, 22),    # Thunderstorm     — 2-digit positive
    (0x05, -22),   # Snow             — 2-digit negative
    (0x06, -40),   # Fog              — min-cap boundary (widest label: "-40°C")
]

# Shortcut icon token combos to cycle through with 'c' (media-mode.md /
# shortcut_icons.cpp). Last entry includes an unknown token deliberately, to
# exercise Ori's "unrecognized token → fall back to a neutral icon" path.
SHORTCUT_COMBOS = [
    ("vol-mute",    "mic-mute",    "screenshot"),   # firmware default
    ("lock-screen", "favorite",    "calculator"),
    ("mic-mute",    "lock-screen", "vol-mute"),
    ("favorite",    "screenshot",  "not-a-real-token"),
]

# Advertising manufacturer-data mode flag (ble-protocol.md §2). Company ID is a
# placeholder; Bleak strips the 2-byte company-ID prefix, so manufacturer_data[
# MFG_COMPANY_ID] is just the flag byte.
MFG_COMPANY_ID   = 0xFFFF
ADV_FLAG_SETUP   = 0x01   # 0 bonded peers — fresh device or post-factory-reset
ADV_FLAG_RUNTIME = 0x02   # ≥1 bonded peer, advertising so it can reconnect

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
    """Encode img to JPEG.

    With --photo-quality set (_FORCED_JPEG_QUALITY), encode once at exactly that
    quality. Otherwise return the highest quality that fits within hard_cap.
    """
    if _FORCED_JPEG_QUALITY is not None:
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=_FORCED_JPEG_QUALITY, optimize=True)
        data = buf.getvalue()
        if len(data) > hard_cap:
            print(f"  [warn] forced quality={_FORCED_JPEG_QUALITY} → {len(data):,} B "
                  f"exceeds hard cap {hard_cap:,} B; sending anyway")
        return data
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

def _crop_resize_to_aspect(img, tw: int, th: int):
    """Center-crop img to the tw:th aspect ratio (cropping whichever axis is
    oversized), then resize to exactly tw×th."""
    w, h = img.size
    if w / h > tw / th:                      # source too wide — crop sides
        nw = int(h * tw / th)
        img = img.crop(((w - nw) // 2, 0, (w + nw) // 2, h))
    else:                                    # source too tall — crop top/bottom
        nh = int(w * th / tw)
        img = img.crop((0, (h - nh) // 2, w, (h + nh) // 2))
    return img.resize((tw, th), _PILImage.LANCZOS)

def build_time_off_photo_jpeg() -> bytes:
    """Crop Time Off photo to 528:396 aspect ratio, resize to 528×396, compress to ≤ 64 KB."""
    if not _PIL_AVAILABLE:
        print("  [warn] Pillow not installed — Time Off photo empty  (pip install Pillow)")
        return b""
    if not os.path.exists(_TIME_OFF_PHOTO_SRC):
        print(f"  [warn] Time Off photo not found: {_TIME_OFF_PHOTO_SRC}")
        return b""
    img  = _PILImage.open(_TIME_OFF_PHOTO_SRC).convert("RGB")
    img  = _crop_resize_to_aspect(img, 528, 396)
    data = _jpeg_max_quality(img, hard_cap=512 * 1024)
    print(f"  [info] Time Off photo: 528×396 JPEG, {len(data):,} B")
    return data

def build_album_art_jpeg(raw_image_bytes: bytes) -> bytes:
    """Crop/resize an arbitrary source image (the Windows now-playing session's
    embedded thumbnail — any format Pillow can decode, usually JPEG or PNG) to
    Ori's Media Album Art target: 484×216, target 15-30 KB, hard cap 64 KB
    (ble-protocol.md §10)."""
    if not _PIL_AVAILABLE or not raw_image_bytes:
        return b""
    img  = _PILImage.open(io.BytesIO(raw_image_bytes)).convert("RGB")
    img  = _crop_resize_to_aspect(img, 484, 216)
    data = _jpeg_max_quality(img, hard_cap=64 * 1024)
    print(f"  [info] album art: 484×216 JPEG, {len(data):,} B")
    return data

# ── Windows now-playing media (ble-protocol.md §12) ────────────────────────────

async def _read_thumbnail_bytes(thumb_ref) -> bytes:
    """Read a WinRT IRandomAccessStreamReference (MediaProperties.thumbnail)
    into raw bytes. Returns b"" if there's no thumbnail."""
    if not thumb_ref:
        return b""
    stream = await thumb_ref.open_read_async()
    size = stream.size
    if size == 0:
        return b""
    buf = _WinBuffer(size)
    await stream.read_async(buf, size, _WinInputStreamOptions.READ_AHEAD)
    return bytes(buf)

def _timespan_to_seconds(ts) -> Optional[int]:
    """Convert a WinRT TimeSpan to whole seconds.

    The winsdk Python binding maps Windows.Foundation.TimeSpan to
    datetime.timedelta — NOT a plain int. The plain-int path (100-ns ticks)
    is kept as a fallback for any winsdk build that differs.
    """
    if ts is None:
        return None
    try:
        if hasattr(ts, 'total_seconds'):          # datetime.timedelta (typical)
            return max(0, int(ts.total_seconds()))
        return int(ts) // 10_000_000              # raw 100-ns ticks (fallback)
    except (TypeError, ValueError):
        return None

async def read_now_playing_media(quiet: bool = False,
                                  fetch_art: bool = True) -> Optional[dict]:
    """Query the Windows GlobalSystemMediaTransportControlsSessionManager for
    the current now-playing session — the same OS API the real Orion app's
    Windows media bridge uses (ble-protocol.md §12). Returns
    {"title", "artist", "can_seek", "is_playing", "position_s", "duration_s",
    "art_jpeg"} (art_jpeg already resized/recompressed to Ori's 484×216 target
    via build_album_art_jpeg()), or None if winsdk isn't installed.

    quiet=True suppresses the per-call status prints.
    fetch_art=False skips reading/re-encoding the thumbnail (art_jpeg comes
    back b"") — used by the polling loop which only needs lightweight fields to
    detect changes; the thumbnail is fetched separately once a change is
    confirmed.

    position_s / duration_s are always fetched (they're cheap — no I/O) and
    are None when the OS doesn't expose timeline info for the session."""
    if not _WINRT_AVAILABLE:
        if not quiet:
            print("  [warn] winsdk not installed — can't read Windows now-playing "
                  "media  (pip install winsdk; Windows only)")
        return None
    mgr = await _MediaManager.request_async()
    session = mgr.get_current_session()
    if not session:
        if not quiet:
            print("  [info] No active media session on Windows (nothing playing)")
        return {"title": "", "artist": "", "can_seek": False,
                "is_playing": False, "position_s": None, "duration_s": None,
                "art_jpeg": b""}

    props    = await session.try_get_media_properties_async()
    playback = session.get_playback_info()

    can_seek = False
    try:
        can_seek = bool(playback.controls.is_playback_position_enabled)
    except Exception:
        pass

    # MediaPlaybackStatus: 0=Closed 1=Opened 2=Changing 3=Stopped 4=Playing 5=Paused
    # winsdk WinRT enums expose their integer value via .value, NOT via __int__,
    # so int(status) raises TypeError and was silently swallowed — is_playing
    # stayed False permanently, meaning Ori's dead-reckoning timer never ran.
    is_playing = False
    try:
        status     = playback.playback_status
        status_int = getattr(status, 'value', None)
        if status_int is None:
            status_int = int(status)    # fallback for older winsdk
        is_playing = (status_int == 4)
    except Exception:
        pass

    # Timeline position and duration — cheap synchronous read, always fetched.
    position_s: Optional[int] = None
    duration_s: Optional[int] = None
    try:
        tl = session.get_timeline_properties()
        if tl is not None:
            dur = _timespan_to_seconds(tl.end_time)
            if dur and dur > 0:
                duration_s = dur
                position_s = _timespan_to_seconds(tl.position) or 0
    except Exception:
        pass

    art_jpeg = b""
    if fetch_art:
        art_raw  = await _read_thumbnail_bytes(props.thumbnail)
        art_jpeg = build_album_art_jpeg(art_raw) if art_raw else b""
    return {
        "title":      props.title or "",
        "artist":     props.artist or "",
        "can_seek":   can_seek,
        "is_playing": is_playing,
        "position_s": position_s,
        "duration_s": duration_s,
        "art_jpeg":   art_jpeg,
    }

# ── Mock data ─────────────────────────────────────────────────────────────────

def _local_posix_tz() -> str:
    """POSIX TZ string for THIS machine's current local UTC offset.

    The Ori firmware feeds the `tz` field straight into newlib's
    setenv("TZ", …)/tzset(), which parses **POSIX** TZ strings — NOT IANA
    names. An IANA name like "America/Los_Angeles" is unparseable and the
    device falls back to UTC. So we emit a fixed-offset POSIX string
    (e.g. "LOC7" = UTC-7, "LOC-2" = UTC+2) reflecting the offset in effect
    right now. No DST transition rule is included — fine for a mock, since
    the device re-derives wall-clock time from epoch + this offset and the
    script re-syncs; the displayed time is correct as of the sync.
    """
    off = datetime.now().astimezone().utcoffset() or timedelta(0)
    # POSIX offset is the value ADDED to local to reach UTC → sign inverted.
    posix_min = -int(off.total_seconds() // 60)
    sign = "-" if posix_min < 0 else ""           # '-' means east of UTC
    hh, mm = divmod(abs(posix_min), 60)
    name = datetime.now().astimezone().tzname() or "LOC"
    if not name.isalpha() or len(name) < 3:       # POSIX std name: ≥3 letters
        name = "LOC"
    return f"{name}{sign}{hh}" + (f":{mm:02d}" if mm else "")

def build_time_sync() -> bytes:
    now_utc = int(datetime.now(timezone.utc).timestamp())
    tx_ms   = int(time.monotonic() * 1000)
    tz      = _local_posix_tz()
    print(f"  [info] TimeSync tz='{tz}' (POSIX; from this machine's local offset)")
    # Keys are single chars (ble-protocol.md §4): u=epoch_utc, z=tz, x=tx_ms.
    return cbor2.dumps({"u": now_utc, "z": tz, "x": tx_ms})

def build_profile() -> bytes:
    # Keys are single chars (ble-protocol.md §10): n=name, t=title, e=email,
    # p=phone. name/title capped at 32 chars, phone at 16 chars (sliced by
    # characters, mirroring the input cap Orion enforces).
    return cbor2.dumps({
        "n": "To Van Hien"[:32],
        "t": "Electrical Engineer"[:32],
        "e": "hientv1999@gmail.com"[:32],
        "p": "+1-778-751-7347"[:16],
    })

def _local_midnight_epoch() -> int:
    """Epoch (UTC) of THIS machine's local midnight, today.

    Used for MeetingList's "d" day-rollover marker (ble-protocol.md §4) and
    for Time Off's start/end window — both want THIS machine's local
    wall-clock midnight, not a naive UTC-day boundary (now - now % 86400),
    which silently shifts by the tester's UTC offset. datetime.timestamp()
    on an aware datetime always returns the correct UTC epoch regardless of
    tzinfo. Meeting start/end times themselves are anchored to "now" instead
    (build_meetings_realistic()), not to this midnight.
    """
    local_now = datetime.now().astimezone()
    local_midnight = local_now.replace(hour=0, minute=0, second=0, microsecond=0)
    return int(local_midnight.timestamp())

def build_meetings() -> bytes:
    return build_meetings_stress() if _STRESS_MEETINGS else build_meetings_realistic()

def build_meetings_realistic() -> bytes:
    # A believable day, not a boundary test — for eyeballing the meeting list
    # screen with content that actually looks like someone's calendar.
    # Anchored to "now" (not fixed wall-clock times) so the list is always
    # relevant regardless of when this script is run: the first meeting
    # starts immediately (exercises the in-progress red highlight right
    # away — meeting-list.md), and the rest follow at realistic spacing.
    # "d" (date) is still local midnight of today — that field is only for
    # day-rollover detection (ble-protocol.md §4), independent of when the
    # meetings themselves start.
    # Keys are single chars: i=id, s=start, e=end, t=title, l=loc, o=org;
    # wrapper: d=date, m=items.
    now      = int(datetime.now(timezone.utc).timestamp())
    midnight = _local_midnight_epoch()
    # (start_offset_min, duration_min, title, loc, org) — offsets relative to "now".
    sample = [
        (0,   30, "Daily Standup",          "Zoom",              "Sarah Chen"),
        (45,  60, "Q3 Roadmap Review",      "Conference Room A", "Mike Torres"),
        (150, 60, "Lunch with Design Team", "Cafeteria",         "Priya Patel"),
        (240, 30, "1:1 with Manager",       "Zoom",              "James Wright"),
        (330, 60, "Sprint Retro",           "Conference Room B", "Sarah Chen"),
        (420, 45, "Client Sync",            "Zoom",              "Priya Patel"),
    ]
    items = []
    for i, (offset_min, dur_min, title, loc, org) in enumerate(sample):
        start = now + offset_min * 60
        items.append({
            "i": f"ev{i:02d}",
            "s": start,
            "e": start + dur_min * 60,
            "t": title,
            "l": loc,
            "o": org,
        })
    return cbor2.dumps({"d": midnight, "m": items})

def build_meetings_stress() -> bytes:
    # 32 meetings (protocol cap) with near-maximum field lengths — boundary
    # test for truncation/buffer limits, not representative of a real day.
    # Per-meeting estimate ~122 B × 32 + 19 B wrapper ≈ 3,923 B — under the
    # firmware boot buffer cap of 4,096 B (main.cpp: static uint8_t meet_buf[4096]).
    midnight = _local_midnight_epoch()
    items = []
    for i in range(32):
        start = midnight + (8 + i // 2) * 3600 + (i % 2) * 1800   # 08:00–23:30
        items.append({
            "i": f"ev{i:02d}",
            "s": start,
            "e": start + 3600,
            "t": (f"Stress Meeting {i:02d}: " + "X" * 40)[:40],
            "l": (f"Room-{i:02d}/Video "      + "X" * 20)[:20],
            "o": (f"Organizer{i:02d} "         + "X" * 15)[:15],
        })
    data = cbor2.dumps({"d": midnight, "m": items})
    if len(data) >= 4096:
        raise ValueError(
            f"meetings CBOR too large: {len(data)} B (firmware boot buffer limit 4096 B)")
    return data

def build_time_off(image: bytes = b"") -> bytes:
    # destination maximized to its 128-byte wire cap (ble-protocol.md §10).
    # Keys are single chars: s=start, e=end, d=destination, m=image.
    #
    # Anchored to local midnight (not raw "now"): a live wall-clock second
    # makes start/end — and therefore the whole entry's hash — different on
    # every call, so Ori's manifest correctly (but unhelpfully, for testing)
    # reports Time Off as changed on every single resync even though nothing
    # actually changed. (Meetings deliberately don't bother with this — see
    # build_meetings_realistic() — since they're RAM-only on Ori with no
    # persisted state to spuriously invalidate.)
    midnight    = _local_midnight_epoch()
    week        = 7 * 86400
    destination = ("Tokyo, Japan — Cherry Blossom Season Adventure " * 3)[:128]
    return cbor2.dumps({
        "s": midnight + week,
        "e": midnight + 2 * week,
        "d": destination,
        "m": image,
    })

def build_device_settings(presence: Optional[int] = None,
                          slot1: Optional[str] = None,
                          slot2: Optional[str] = None,
                          slot3: Optional[str] = None,
                          clock_face: Optional[int] = None,
                          time_format: Optional[int] = None,
                          ancs_filter: Optional[int] = None,
                          weather_condition: Optional[int] = None,
                          temp_c: Optional[int] = None,
                          temp_unit: Optional[int] = None) -> bytes:
    # Device Settings (char 000E) — all fields optional; absent keys leave
    # Ori's current state unchanged. Applied immediately outside BEGIN/END.
    # Keys: "p"=presence, "1"/"2"/"3"=shortcut slots, "c"=clock_face,
    # "h"=time_format (0=24-hour, 1=12-hour), "f"=ancs_filter,
    # "w"=weather_condition, "d"=temperature, "u"=temp_unit (0=Fahrenheit,
    # 1=Celsius — this mock always declares Celsius, see WEATHER_CYCLE).
    # Firmware only applies weather when "w", "d", AND "u" are all present in
    # the same write — always pass all three together.
    d: dict = {}
    if presence is not None:
        d["p"] = presence
    if slot1 is not None:
        d["1"] = slot1
    if slot2 is not None:
        d["2"] = slot2
    if slot3 is not None:
        d["3"] = slot3
    if clock_face is not None:
        d["c"] = clock_face
    if time_format is not None:
        d["h"] = time_format
    if ancs_filter is not None:
        d["f"] = ancs_filter
    if weather_condition is not None:
        d["w"] = weather_condition
    if temp_c is not None:
        d["d"] = temp_c
    if temp_unit is not None:
        d["u"] = temp_unit
    return cbor2.dumps(d)

def build_host_volume_state(level: int, mute: bool = False) -> bytes:
    # Single-char keys (ble-protocol.md §4): l=level (0..100), m=mute.
    return cbor2.dumps({"l": max(0, min(100, int(level))), "m": bool(mute)})

def build_media_metadata(title: str, artist: str, can_seek: bool = False,
                          playing: bool = False,
                          position_s: Optional[int] = None,
                          duration_s: Optional[int] = None) -> bytes:
    # Keys are single chars (ble-protocol.md §4):
    #   t=title, a=artist, c=can_seek, p=playing, o=position_s, d=duration_s.
    # "p" is always included so Ori's play icon stays in sync with the OS —
    # the firmware treats absent "p" as "no change" (forward-compat), but the
    # mock always sends it for clarity. "o"+"d" are included only when both
    # are provided (track change or explicit seek), letting Ori reset its
    # dead-reckoning timer to the correct anchor; otherwise Ori advances on
    # its own 1-second tick.
    d: dict = {"t": (title or "")[:192], "a": (artist or "")[:96]}
    if can_seek:
        d["c"] = True
    d["p"] = bool(playing)
    if position_s is not None and duration_s is not None and duration_s > 0:
        d["o"] = int(position_s)
        d["d"] = int(duration_s)
    return cbor2.dumps(d)

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
        self._seq             = 0   # SyncControl seq, bumped each run_sync()
        # Slot → token map, mirroring what was last sent to Ori via ShortcutConfig.
        # Initialized to firmware defaults; updated by run_sync() and push_shortcuts().
        self._shortcut_slots: dict[int, str] = {1: "vol-mute", 2: "mic-mute", 3: "screenshot"}
        # In-flight album-art chunked-write task, if any — see
        # _spawn_album_art_push()'s doc comment. Mirrors central.rs's
        # BleState::album_art_task (the real Orion app's own equivalent).
        self._album_art_task: Optional[asyncio.Task] = None

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
            self.needed = msg.get("n", [])  # "n" = needed (ble-protocol.md §4)
            print(f"\n  [notify] SyncManifest ← needed={self.needed}")
        except Exception:
            print(f"\n  [notify] SyncManifest ← raw {data.hex()}")
        self._manifest_reply.set()

    def _on_phone_bond_status(self, _: BleakGATTCharacteristic, data: bytearray):
        # ble-protocol.md §3/§4/§11 — Ori → Orion notify + readable on char
        # 000F. Fires on every iPhone connect/disconnect/unpair (name/type/
        # signal/battery reset) as well as live queue/RSSI/battery changes
        # while connected — this is the "does Orion get told the iPhone
        # reconnected" signal.
        try:
            msg = cbor2.loads(bytes(data))
            bonded    = msg.get("b", False)
            connected = msg.get("c", False)
            name      = msg.get("n", "")
            dtype     = msg.get("d", "")
            missed    = msg.get("m", 0)
            unread    = msg.get("u", 0)
            total     = msg.get("t", 0)
            signal    = msg.get("s", 0)
            battery   = msg.get("l", 0)
            state = "connected" if connected else ("bonded, disconnected" if bonded else "not bonded")
            print(f"\n  [notify] PhoneBondStatus ← {state} name='{name}' type='{dtype}' "
                  f"missed={missed} unread={unread} total={total} signal={signal} battery={battery}%")
        except Exception as exc:
            print(f"\n  [notify] PhoneBondStatus ← parse error: {exc}")

    def _on_keyboard_command(self, _: BleakGATTCharacteristic, data: bytearray):
        # ble-protocol.md §12 — Ori → Orion notify on char 000B.
        # Dispatch to the async handler so we can call WinRT session APIs.
        try:
            msg = cbor2.loads(bytes(data))
            op  = msg.get("o", "")
            arg = int(msg.get("a", 0))
            print(f"\n  [notify] KeyboardCommand ← op='{op}' arg={arg}")
            asyncio.ensure_future(self._handle_keyboard_command(op, arg))
        except Exception as exc:
            print(f"\n  [notify] KeyboardCommand ← parse error: {exc}")

    async def _handle_keyboard_command(self, op: str, arg: int):
        """Bridge an Ori KeyboardCommand to OS APIs (ble-protocol.md §12).

          shortcut   → look up token for slot arg in _shortcut_slots, run action
          vol_set    → set Windows master volume via pycaw, write HostVolumeState back
          play_pause → GlobalSystemMediaTransportControls (winsdk required)
          next / prev / seek → same
        """
        loop = asyncio.get_event_loop()
        try:
            if op == "shortcut":
                token = self._shortcut_slots.get(arg, "")
                print(f"  [kbd] shortcut slot {arg} ({token!r})")
                if token == "vol-mute":
                    state = await loop.run_in_executor(None, _toggle_master_mute_sync)
                    print(f"  [kbd] vol-mute → {state}")
                elif token == "mic-mute":
                    state = await loop.run_in_executor(None, _toggle_mic_mute_sync)
                    print(f"  [kbd] mic-mute → {state}")
                elif token == "screenshot":
                    await loop.run_in_executor(None, _trigger_screenshot_sync)
                    print("  [kbd] screenshot → Print Screen sent")
                elif token == "lock-screen":
                    await loop.run_in_executor(None, _lock_screen_sync)
                    print("  [kbd] lock-screen → LockWorkStation called")
                elif token == "calculator":
                    await loop.run_in_executor(None, _open_calculator_sync)
                    print("  [kbd] calculator → Calculator launched")
                elif token == "favorite":
                    print("  [kbd] favorite → user-configured action (no-op in mock)")
                elif not token:
                    print(f"  [kbd] slot {arg} not configured")
                else:
                    print(f"  [kbd] unknown token '{token}'")
                return

            if op == "vol_set":
                # Set Windows master volume, then write HostVolumeState back to Ori
                # as confirmation. Ori ignores the write during its 800 ms
                # swipe-override window (ble-protocol.md §12).
                actual = await loop.run_in_executor(None, _set_windows_volume_sync, arg)
                if actual is not None:
                    print(f"  [kbd] vol_set → Windows volume = {actual}%")
                    await self.push_host_volume(actual)
                else:
                    print(f"  [kbd] vol_set {arg}% — pycaw not installed "
                          "(pip install pycaw; Windows only)")
                return

            # Media transport ops require winsdk.
            if not _WINRT_AVAILABLE:
                return
            mgr     = await _MediaManager.request_async()
            session = mgr.get_current_session()
            if not session:
                print("  [kbd] no active media session — command ignored")
                return
            if op == "play_pause":
                await session.try_toggle_play_pause_async()
                print("  [kbd] play_pause sent to OS")
            elif op == "next":
                await session.try_skip_next_async()
                print("  [kbd] next sent to OS")
            elif op == "prev":
                await session.try_skip_previous_async()
                print("  [kbd] prev sent to OS")
            elif op == "seek":
                # WinRT TimeSpan = 100-nanosecond ticks; arg is seconds.
                await session.try_change_playback_position_async(arg * 10_000_000)
                print(f"  [kbd] seek to {arg}s sent to OS")
            else:
                print(f"  [kbd] unknown op '{op}'")
        except Exception as exc:
            print(f"  [kbd] error handling '{op}': {exc}")

    # ── helpers ──

    async def wait_status_in(self, targets, timeout: float = 30.0) -> bool:
        deadline = time.monotonic() + timeout
        while True:
            if self.last_status in targets:
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

    # In-flight fragment window between Write-With-Response checkpoints. Bounds
    # how far the fast Write-No-Response stream can run ahead of Ori draining its
    # RX buffer (32 × 238 B ≈ 7.6 KB). See ble-protocol.md §5 flow control.
    CHUNK_WINDOW = 32

    async def write_chunked(self, uuid: str, payload: bytes, label: str):
        frames = make_frames(payload)
        suffix = "empty" if not payload else f"{len(payload):,} B → {len(frames)} frame(s)"
        print(f"  [chunk]  {label}: {suffix}")
        # Stream fragments Write-No-Response for speed (no per-write ATT ack),
        # with a Write-With-Response checkpoint every CHUNK_WINDOW frames (and on
        # the last frame). The checkpoint's ack confirms every prior fragment
        # landed and paces the sender so a 2M-PHY/WNR burst can't overrun Ori.
        # Chars 0005/0006/0007 advertise WRITE_NR as of proto 1.2.
        n = len(frames)
        for i, frame in enumerate(frames):
            checkpoint = ((i + 1) % self.CHUNK_WINDOW == 0) or (i == n - 1)
            await self.client.write_gatt_char(uuid, bytearray(frame), response=checkpoint)

    async def write_chunked_nr(self, uuid: str, payload: bytes, label: str):
        # Media Album Art (000E) advertises WRITE_NR only — no Write (with-
        # response) property at all (gatt_server.cpp), so the write_chunked()
        # checkpoint above isn't available here. Every frame is Write-No-
        # Response; the only throttle against overrunning Ori's RX buffer is a
        # short pause every CHUNK_WINDOW frames. Album art is small (≤ 64 KB
        # hard cap) and non-critical (a stale/missing frame just means a
        # blank/old art tile, not a broken sync), so this is acceptable for a
        # test tool — the real Orion app should request WRITE on this
        # characteristic if it needs the same ack-based pacing as the other
        # chunked characteristics.
        frames = make_frames(payload)
        suffix = "empty" if not payload else f"{len(payload):,} B → {len(frames)} frame(s)"
        print(f"  [chunk]  {label}: {suffix} (write-no-response only)")
        for i, frame in enumerate(frames):
            await self.client.write_gatt_char(uuid, bytearray(frame), response=False)
            if (i + 1) % self.CHUNK_WINDOW == 0:
                await asyncio.sleep(0.02)

    async def subscribe(self):
        await self.client.start_notify(UUID_DEV_STATUS,  self._on_dev_status)
        await self.client.start_notify(UUID_SYNC_CTRL,   self._on_sync_ctrl)
        await self.client.start_notify(UUID_MANIFEST,    self._on_manifest)
        # Subscribe to Keyboard Command notifies — Ori sends play_pause / next /
        # prev / seek / vol_set / shortcut here; we bridge them to OS APIs.
        await self.client.start_notify(UUID_KEYBOARD_CMD, self._on_keyboard_command)
        # Subscribe to Phone Bond Status — Ori's iPhone ANCS bond/connection
        # state (ble-protocol.md §3/§11). Read once up front (same "seed
        # current value" treatment as Device Status below) so we see whatever
        # state Ori is already in, then relay every subsequent notify —
        # mirrors the real Orion app's phone_bond_watcher() in central.rs.
        await self.client.start_notify(UUID_PHONE_STATUS, self._on_phone_bond_status)
        raw = await self.client.read_gatt_char(UUID_DEV_STATUS)
        self._on_dev_status(None, raw)  # seed current value
        phone_raw = await self.client.read_gatt_char(UUID_PHONE_STATUS)
        self._on_phone_bond_status(None, phone_raw)  # seed current value

    async def push_presence(self, value: int):
        # Presence is "p" in Device Settings (char 000E) — write-only, applied
        # immediately outside BEGIN/END. Must push fresh on every connect.
        name = PRESENCE_NAMES.get(value, f"0x{value:02X}")
        payload = build_device_settings(presence=value)
        await self.write(UUID_DEV_SETTINGS, payload, f"DeviceSettings presence={name}")

    async def push_weather(self, condition: int, temp_c: int):
        # Weather is "w"/"d"/"u" in Device Settings (char 000E) — ephemeral
        # like presence, always written together (firmware ignores a write
        # with only some of the three — ble-protocol.md §6.4). Must push
        # fresh on every connect. This mock always declares Celsius (u=1).
        name = WEATHER_NAMES.get(condition, f"0x{condition:02X}")
        payload = build_device_settings(weather_condition=condition, temp_c=temp_c, temp_unit=1)
        await self.write(UUID_DEV_SETTINGS, payload,
                         f"DeviceSettings weather={name} temp_c={temp_c} unit=C")

    async def push_clock_face(self, value: int):
        # Clock Face is "c" in Device Settings — persisted to NVS; only write
        # when the user changes it, not on every reconnect.
        name = CLOCK_FACE_NAMES.get(value, f"0x{value:02X}")
        payload = build_device_settings(clock_face=value)
        await self.write(UUID_DEV_SETTINGS, payload, f"DeviceSettings clock_face={name}")

    async def push_time_format(self, value: int):
        # Time Format is "h" in Device Settings — persisted to NVS; only write
        # when the user changes it, not on every reconnect.
        name = TIME_FORMAT_NAMES.get(value, f"0x{value:02X}")
        payload = build_device_settings(time_format=value)
        await self.write(UUID_DEV_SETTINGS, payload, f"DeviceSettings time_format={name}")

    async def push_ancs_filter(self, value: int):
        # ANCS filter is "f" in Device Settings — persisted to NVS; only write
        # when the user changes it.
        name = ANCS_FILTER_NAMES.get(value, f"0x{value:02X}")
        payload = build_device_settings(ancs_filter=value)
        await self.write(UUID_DEV_SETTINGS, payload, f"DeviceSettings ancs_filter={name}")

    async def read_device_settings(self) -> dict:
        # Read Device Settings (char 000E) to recover the NVS-persisted fields:
        #   "c" = clock_face (0=Digital, 1=Analog)
        #   "h" = time_format (0=24-hour, 1=12-hour)
        #   "f" = ancs_filter (0=Disabled, 1=CallOnly, 2=Important, 3=All)
        #   "1"/"2"/"3" = shortcut slot tokens
        # Presence and weather are NOT returned — Orion is the source of truth
        # for both. ble-protocol.md §6.4 / §4 DeviceSettings schema.
        try:
            raw = await self.client.read_gatt_char(UUID_DEV_SETTINGS)
            msg = cbor2.loads(bytes(raw))
            cf  = msg.get("c")
            tf  = msg.get("h")
            af  = msg.get("f")
            cf_name = CLOCK_FACE_NAMES.get(cf,   f"0x{cf:02X}" if isinstance(cf, int) else "?")
            tf_name = TIME_FORMAT_NAMES.get(tf,  f"0x{tf:02X}" if isinstance(tf, int) else "?")
            af_name = ANCS_FILTER_NAMES.get(af,   f"0x{af:02X}" if isinstance(af, int) else "?")
            print(f"  [read] DeviceSettings: clock_face={cf_name}  time_format={tf_name}  ancs_filter={af_name}")
            return msg
        except Exception as exc:
            print(f"  [read] DeviceSettings: error {exc}")
            return {}

    async def push_shortcuts(self, slot1: str, slot2: str, slot3: str):
        # Shortcut slots are "1"/"2"/"3" in Device Settings — written outside
        # the BEGIN/END staging pipeline; applied immediately on Ori.
        self._shortcut_slots = {1: slot1, 2: slot2, 3: slot3}
        payload = build_device_settings(slot1=slot1, slot2=slot2, slot3=slot3)
        print(f"\n  [info] pushing shortcuts: {slot1}, {slot2}, {slot3}")
        await self.write(UUID_DEV_SETTINGS, payload,
                         f"DeviceSettings shortcuts={slot1},{slot2},{slot3}")

    async def push_host_volume(self, level: int, mute: bool = False):
        # HostVolumeState (char 000C) is Write (response) — Orion is the sole
        # writer. Ori ignores writes during the 800 ms swipe-override window
        # (ble-protocol.md §12 drag-wins rule) but accepts them afterwards.
        payload = build_host_volume_state(level, mute)
        await self.client.write_gatt_char(UUID_HOST_VOLUME, bytearray(payload), response=True)
        print(f"  [vol]  HostVolumeState → Ori: level={level}% mute={mute}")

    async def push_time_sync(self):
        # Time Sync is inside the BEGIN/END pipeline (RAM-only on Ori, no hash,
        # but still staged — ble-protocol.md §6.0). Wrap it in its own session.
        time_sync_bytes = build_time_sync()
        print(f"\n  [info] pushing time sync")
        self._seq += 1
        seq   = self._seq
        total = len(time_sync_bytes)
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"o": "BEGIN", "s": seq, "t": total}),
            f"SyncControl BEGIN seq={seq} total={total}")
        await self.write(UUID_TIME_SYNC, time_sync_bytes, "TimeSync")
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"o": "END", "s": seq}),
            f"SyncControl END seq={seq}")
        if await self.wait_status_in({0x10, 0x03}, timeout=15.0):
            print("  [ok] Time sync complete.\n")
        else:
            print("  [warn] Time sync completion not seen.\n")

    async def push_media(self):
        info = await read_now_playing_media()
        if info is None:
            return  # winsdk missing — already warned
        await self._send_media(info)

    async def _send_media(self, info: dict):
        # Media Metadata (000D) and Media Album Art (000E) are applied
        # immediately on write — unlike Time/Profile/Photo/Meetings/Time Off, they
        # are NOT staged behind SyncControl{BEGIN..END} (gatt_server.cpp's
        # handle_media_metadata/handle_album_art call straight into app_state,
        # no g_stage involved). PSRAM-cached only, no NVS, no hash, no
        # manifest, no blackout (ble-protocol.md §12) — so this is just two
        # bare writes, no BEGIN/END session needed. Shared by push_media()
        # (manual 'm') and media_watcher() (auto-push on change).
        is_playing = info.get("is_playing", False)
        position_s = info.get("position_s")
        duration_s = info.get("duration_s")
        pos_str    = (f"  pos={position_s}s/{duration_s}s"
                      if position_s is not None and duration_s else "")
        print(f"\n  [info] now playing: title='{info['title']}' "
              f"artist='{info['artist']}' can_seek={info['can_seek']} "
              f"{'▶' if is_playing else '⏸'}{pos_str}")
        meta_bytes = build_media_metadata(
            info["title"], info["artist"], info["can_seek"],
            playing=is_playing,
            position_s=position_s,
            duration_s=duration_s,
        )
        await self.write(UUID_MEDIA_META, meta_bytes, "MediaMetadata")

        art_jpeg = info.get("art_jpeg", b"")
        self._push_art(art_jpeg)

    def _push_art(self, art_jpeg: bytes):
        # Cancels any still-in-flight album art transfer and starts this one
        # immediately instead of awaiting it inline — mirrors central.rs's
        # spawn_album_art_push()/BleState::album_art_task (the real Orion
        # app's own fix for this). Without this, a track change arriving
        # while a previous track's ~15-30 KB art is still chunking out (a
        # few hundred ms at the write-no-response pacing above) just queued
        # behind it: media_watcher()/push_media() both used to `await` the
        # whole chunked write inline, so the old image finished streaming to
        # completion before the new one ever started. Ori's own receiver
        # already resets on the new transfer's first fragment
        # (chunked_transfer.cpp resets on seq==0), so cancelling the old
        # asyncio.Task here is enough — no protocol-level cancel needed.
        if self._album_art_task and not self._album_art_task.done():
            self._album_art_task.cancel()
        if not art_jpeg:
            self._album_art_task = None
            print("  [info] no album art available — sent metadata only.\n")
            return
        self._album_art_task = asyncio.create_task(self._run_album_art_push(art_jpeg))

    async def _run_album_art_push(self, art_jpeg: bytes):
        await self.write_chunked_nr(UUID_ALBUM_ART, art_jpeg, "MediaAlbumArt")
        print("  [ok] Media metadata + album art pushed.\n")

    def _print_hashes(self, profile, photo, meetings, time_off):
        # Device Settings (shortcuts/presence) has no hash entry — written
        # outside BEGIN/END, never staged, so there's nothing to hash-compare.
        print("── SHA-256 hashes (reference for next reconnect) ──")
        print(f"  profile:  {sha256(profile).hex()}")
        print(f"  photo:    {sha256(photo).hex()}")
        print(f"  meetings: {sha256(meetings).hex()}")
        print(f"  time_off: {sha256(time_off).hex()}")

    # ── Unified sync (hash-driven delta) — §6.1 + §6.2 in one flow ────────────
    #
    # ONE flow for first-time setup AND reconnect. We hand Ori a manifest of every
    # section's hash; Ori replies with the subset that differs and we send only
    # those — everything on a first pair (Ori has nothing), or just the changed /
    # dropped sections on a reconnect (e.g. meetings + time after a power cycle,
    # since meetings are RAM-only on Ori). Time Sync is ALWAYS sent (clock drifts
    # and is lost on power cycle). Completion is SETUP_SYNC_COMPLETE (0x03) on a
    # first pair or RUNTIME_READY (0x10) on a reconnect — we accept either.

    async def run_sync(self):
        print("\n═══ Sync (hash-driven delta) ═══")

        # Build every payload up front: needed to hash for the manifest, and so
        # the BEGIN→END burst never stalls building JPEGs mid-transfer (keeps a
        # first-pair handshake well within Ori's BEGIN deadline).
        print("── Building payload ──")
        time_sync_bytes = build_time_sync()
        profile_bytes   = build_profile()
        photo_bytes     = build_profile_photo_jpeg()
        meetings_bytes  = build_meetings()
        time_off_image  = build_time_off_photo_jpeg()
        time_off_bytes  = build_time_off(image=time_off_image)
        self._shortcut_slots = {1: "vol-mute", 2: "mic-mute", 3: "screenshot"}

        # Device Settings: write shortcuts (+ presence already pushed by caller)
        # OUTSIDE the BEGIN/END pipeline — applied immediately on Ori (§6.4).
        dev_settings_bytes = build_device_settings(
            slot1="vol-mute", slot2="mic-mute", slot3="screenshot")
        await self.write(UUID_DEV_SETTINGS, dev_settings_bytes,
                         "DeviceSettings shortcuts")

        # Ask Ori which sections differ from what it already holds.
        print("\n── Sync Manifest (section hashes) ──")
        self._manifest_reply.clear()
        self.needed = []
        # Keys are single chars (ble-protocol.md §4):
        # p=profile_sha, h=photo_sha, m=meetings_sha, t=to_sha.
        # No Device Settings entry — shortcuts/presence are written outside
        # BEGIN/END (char 000E, applied immediately), so they have no hash entry.
        await self.write(UUID_MANIFEST, cbor2.dumps({
            "p": sha256(profile_bytes),
            "h": sha256(photo_bytes),
            "m": sha256(meetings_bytes),
            "t": sha256(time_off_bytes),
        }), "SyncManifest")
        print("── Waiting for Ori manifest reply ──")
        manifest_timed_out = False
        try:
            await asyncio.wait_for(self._manifest_reply.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            manifest_timed_out = True
            print("  [warn] No manifest reply — assuming all sections needed")
            self.needed = ["profile", "photo", "meetings", "to"]

        item_bytes = {
            "profile":   profile_bytes,
            "photo":     photo_bytes,
            "meetings":  meetings_bytes,
            "to":        time_off_bytes,
        }

        # Stale vs. up-to-date breakdown — what Ori actually said it needs,
        # vs. what it already has cached and matches (so we're not sending it).
        stale   = [k for k in item_bytes if k in self.needed]
        in_sync = [k for k in item_bytes if k not in self.needed]
        print("\n── Manifest result ──")
        if manifest_timed_out:
            print("  (no manifest reply — treating every group as stale)")
        if stale:
            print("  STALE     (will send): "
                  + ", ".join(f"{k}({len(item_bytes[k]):,}B)" for k in stale))
        else:
            print("  STALE     (will send): none")
        if in_sync:
            print("  UP TO DATE (skipping): " + ", ".join(in_sync))
        else:
            print("  UP TO DATE (skipping): none")

        # Time Sync rides inside every BEGIN→END unconditionally (RAM-only on
        # Ori, no hash, no manifest gating). Shortcuts now go via Device Settings
        # outside BEGIN/END (already sent above). Only mismatching hash-gated
        # sections (profile/photo/meetings/time_off) are conditional.
        total = len(time_sync_bytes) + sum(
            len(item_bytes[k]) for k in self.needed if k in item_bytes)
        print(f"\n  [info] syncing: time"
              + "".join(f" + {k}({len(item_bytes[k]):,}B)"
                        for k in self.needed if k in item_bytes)
              + f"   → total {total:,} B")

        self._seq += 1
        seq = self._seq
        print("\n── Writing sync data ──")
        # Keys are single chars (ble-protocol.md §4): o=op, s=seq, t=total.
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"o": "BEGIN", "s": seq, "t": total}),
            f"SyncControl BEGIN seq={seq} total={total:,}")
        await self.write(UUID_TIME_SYNC, time_sync_bytes, "TimeSync")
        if "profile"  in self.needed:
            await self.write(UUID_PROFILE, profile_bytes, "ProfileInfo")
        if "photo"    in self.needed:
            await self.write_chunked(UUID_PHOTO, photo_bytes, "ProfilePhoto")
        if "meetings" in self.needed:
            await self.write_chunked(UUID_MEETINGS, meetings_bytes, "MeetingList")
        if "to"       in self.needed:
            await self.write_chunked(UUID_TIME_OFF, time_off_bytes, "TimeOffEntry")
        await self.write(UUID_SYNC_CTRL,
            cbor2.dumps({"o": "END", "s": seq}), f"SyncControl END seq={seq}")

        # First pair → SETUP_SYNC_COMPLETE (0x03); reconnect → RUNTIME_READY (0x10).
        print("\n── Waiting for sync to complete ──")
        if await self.wait_status_in({0x03, 0x10}, timeout=30.0):
            print(f"  [ok] Sync complete (status {DS.get(self.last_status, '?')}).\n")
        else:
            print(f"  [warn] completion not seen "
                  f"(last: {DS.get(self.last_status, '?')})\n")

        self._print_hashes(profile_bytes, photo_bytes, meetings_bytes, time_off_bytes)

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
        print("     Find 'Ori' → click '...' → Remove device")
        print("  2. Wait ~5 s for Ori to finish rebooting (Welcome screen)")
        print("  3. Press 's' here to run the setup sync")
        print("  ────────────────────────────────────────────────────────────")
        print()

        self.disconnected = True  # signal the command loop to reconnect

# ── Background media watcher ───────────────────────────────────────────────────

# ── Windows volume helpers (pycaw — IAudioEndpointVolume) ────────────────────
# These are synchronous COM calls — always run via loop.run_in_executor() so
# they don't block the asyncio event loop.

def _vol_interface():
    """Return IAudioEndpointVolume for the default render endpoint.

    pycaw's AudioUtilities.GetSpeakers() returns either a raw COM IMMDevice
    (older pycaw ≤ 5.x) or an AudioDevice wrapper whose ._dev attribute is the
    raw IMMDevice (newer pycaw). getattr(…, '_dev', …) handles both cases.
    """
    devices = _AudioUtils.GetSpeakers()
    dev     = getattr(devices, '_dev', devices)   # unwrap AudioDevice if needed
    iface   = dev.Activate(_IAudioEPVol._iid_, _CLSCTX_ALL, None)
    return _ctypes_cast(iface, _POINTER(_IAudioEPVol))

def _get_windows_volume_sync() -> Optional[int]:
    """Read Windows master volume as 0..100 (sync; call via run_in_executor)."""
    if not _PYCAW_AVAILABLE:
        return None
    try:
        return round(_vol_interface().GetMasterVolumeLevelScalar() * 100)
    except Exception:
        return None

def _set_windows_volume_sync(level: int) -> Optional[int]:
    """Set Windows master volume to 0..100 (sync; call via run_in_executor).
    Returns the clamped level actually applied, or None on error."""
    if not _PYCAW_AVAILABLE:
        return None
    try:
        level = max(0, min(100, int(level)))
        _vol_interface().SetMasterVolumeLevelScalar(level / 100.0, None)
        return level
    except Exception as exc:
        print(f"  [vol] SetMasterVolumeLevelScalar error: {exc}")
        return None

# ── Shortcut action helpers (sync; call via run_in_executor) ──────────────────
# These implement the five shortcut tokens from ble-protocol.md §12 / media-mode.md.

def _toggle_mute(iface) -> str:
    """Flip GetMute()/SetMute() on an IAudioEndpointVolume. Returns the new state."""
    new_mute = not iface.GetMute()
    iface.SetMute(new_mute, None)
    return "muted" if new_mute else "unmuted"

def _toggle_master_mute_sync() -> str:
    """Toggle Windows master mute. Returns a short description of the new state."""
    if not _PYCAW_AVAILABLE:
        return "pycaw not installed (pip install pycaw; Windows only)"
    try:
        return _toggle_mute(_vol_interface())
    except Exception as exc:
        return f"error: {exc}"

def _toggle_mic_mute_sync() -> str:
    """Toggle the default Windows microphone mute via pycaw."""
    if not _PYCAW_AVAILABLE:
        return "pycaw not installed (pip install pycaw; Windows only)"
    try:
        mic = _AudioUtils.GetMicrophone()
        if mic is None:
            return "no microphone found"
        iface = mic.Activate(_IAudioEPVol._iid_, _CLSCTX_ALL, None)
        return _toggle_mute(_ctypes_cast(iface, _POINTER(_IAudioEPVol)))
    except Exception as exc:
        return f"error: {exc}"

def _trigger_screenshot_sync() -> None:
    """Open Snipping Tool via Win+Shift+S (Windows 10/11)."""
    VK_LWIN         = 0x5B
    VK_SHIFT        = 0x10
    VK_S            = 0x53
    KEYEVENTF_KEYUP = 0x0002
    ctypes.windll.user32.keybd_event(VK_LWIN,  0, 0,                0)
    ctypes.windll.user32.keybd_event(VK_SHIFT, 0, 0,                0)
    ctypes.windll.user32.keybd_event(VK_S,     0, 0,                0)
    ctypes.windll.user32.keybd_event(VK_S,     0, KEYEVENTF_KEYUP, 0)
    ctypes.windll.user32.keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0)
    ctypes.windll.user32.keybd_event(VK_LWIN,  0, KEYEVENTF_KEYUP, 0)

def _lock_screen_sync() -> None:
    """Lock the Windows workstation (equivalent to Win+L)."""
    ctypes.windll.user32.LockWorkStation()

def _open_calculator_sync() -> None:
    """Launch Windows Calculator."""
    os.startfile("calc.exe")

# How often to poll Windows for a track change. winsdk/WinRT does expose native
# change events (add_media_properties_changed), but those fire callbacks on a
# non-asyncio COM thread — bridging them into this script's event loop reliably
# is much more involved than a cheap poll, and a track change is not latency-
# critical enough to justify it. Polling only fetches title/artist/can_seek
# (fetch_art=False) until a change is actually detected, so steady-state cost
# is one lightweight WinRT call per interval, not a JPEG re-encode.
MEDIA_WATCH_INTERVAL_S = 2.0

async def media_watcher(orion: MockOrion):
    """Background task: polls Windows now-playing and pushes MediaMetadata +
    MediaAlbumArt on three kinds of change:

    1. Track change (title/artist/can_seek differ) — re-fetch with art.
    2. Play-state change — push metadata+position immediately, skip art re-encode.
    3. PC-side seek — detected when actual position deviates more than 5 s from
       the dead-reckoned expected position (last_pos + elapsed) while both polls
       show is_playing=True. Pushes updated position so Ori's dead-reckoning
       timer resets to the correct anchor (ble-protocol.md §4 MediaMetadata)."""
    if not _WINRT_AVAILABLE:
        return  # 'm' warns once if the user tries it; stay quiet here
    loop = asyncio.get_event_loop()

    last_track_key   = (None, None, None)  # (title, artist, can_seek)
    last_play_key    = None                 # is_playing
    last_position_s: Optional[int] = None  # position at last push / poll
    last_poll_time:  float = 0.0           # loop.time() of last poll

    while not orion.disconnected and orion.client.is_connected:
        t_now = loop.time()
        info  = await read_now_playing_media(quiet=True, fetch_art=False)
        if info is None:
            return
        track_key = (info["title"], info["artist"], info["can_seek"])
        play_key  = info["is_playing"]
        cur_pos   = info.get("position_s")

        track_changed = (track_key != last_track_key)
        play_changed  = (play_key  != last_play_key)

        # Seek detection: both this and the previous poll must be playing, and
        # the actual position must deviate more than 5 s from what dead-
        # reckoning predicts (last_pos + elapsed).  Only runs when the track
        # hasn't changed — a new track resets everything via track_changed.
        seek_detected = False
        if (not track_changed
                and last_play_key and play_key          # both polls: playing
                and last_position_s is not None
                and cur_pos is not None
                and last_poll_time > 0):
            expected = last_position_s + (t_now - last_poll_time)
            if abs(cur_pos - expected) > 5:
                seek_detected = True

        # Advance tracking state before the (potentially slow) _send_media call.
        last_poll_time = t_now
        last_play_key  = play_key
        if cur_pos is not None:
            last_position_s = cur_pos

        if track_changed or play_changed or seek_detected:
            last_track_key = track_key
            icon = '▶' if play_key else '⏸'
            if track_changed:
                print(f"\n  [watch] track → '{info['title']}' — {info['artist']} {icon}")
                full = await read_now_playing_media(quiet=True, fetch_art=True)
                if full is None:
                    return
                await orion._send_media(full)
                # Update position anchor from the more accurate full-fetch result.
                if full.get("position_s") is not None:
                    last_position_s = full["position_s"]
                    last_poll_time  = loop.time()
            elif seek_detected:
                print(f"\n  [watch] PC seek → {cur_pos}s {icon}")
                await orion._send_media(info)
            else:
                print(f"\n  [watch] play state → {icon}")
                await orion._send_media(info)

        await asyncio.sleep(MEDIA_WATCH_INTERVAL_S)

# ── Background volume watcher ─────────────────────────────────────────────────

# How often to poll Windows master volume for changes. pycaw's COM call is
# lightweight (no I/O), so 1 s is fine. This is the same role as Orion's
# IAudioEndpointVolumeCallback (debounced ~100 ms) — here we poll instead.
VOLUME_WATCH_INTERVAL_S = 1.0

async def volume_watcher(orion: MockOrion):
    """Background task: polls Windows master volume and pushes HostVolumeState
    to Ori on every change — mirrors how the real Orion app's
    IAudioEndpointVolumeCallback detects OS volume changes and writes back
    (ble-protocol.md §12). The first poll runs immediately (no initial sleep)
    so the correct level is pushed on connect/reconnect before the user
    starts swiping. Windows only; degrades silently if pycaw is not installed.

    Ori ignores HostVolumeState writes during its 800 ms swipe-override window
    (set when a vertical-swipe starts; cleared on release), so concurrent
    watcher pushes during a drag are harmlessly dropped on the firmware side."""
    if not _PYCAW_AVAILABLE:
        return
    loop = asyncio.get_event_loop()
    last_level: Optional[int] = None
    while not orion.disconnected and orion.client.is_connected:
        level = await loop.run_in_executor(None, _get_windows_volume_sync)
        if level is not None and level != last_level:
            if last_level is None:
                print(f"\n  [vol] initial volume: {level}%")
            else:
                print(f"\n  [vol] volume changed {last_level}% → {level}%")
            last_level = level
            try:
                await orion.push_host_volume(level)
            except Exception as exc:
                print(f"  [vol] push HostVolumeState error: {exc}")
        await asyncio.sleep(VOLUME_WATCH_INTERVAL_S)

# ── Interactive command loop ──────────────────────────────────────────────────

MENU = """
Commands:
  s — sync now        (hash-driven delta; also runs automatically on connect)
  p — push presence   (cycle AVAILABLE -> BUSY -> AWAY -> OFFLINE -> ...)
  c — push shortcuts  (cycle through SHORTCUT_COMBOS test sets)
  t — resync time     (manual TimeSync-only push, inside BEGIN/END)
  m — push media now  (manual one-shot; media also auto-pushes in the
                        background on every track change — see media_watcher)
  k — push clock face (toggle DIGITAL <-> ANALOG, char 000E Device Settings)
  h — push time format (toggle 24-HOUR <-> 12-HOUR, char 000E Device Settings)
  n — push ANCS filter (cycle DISABLED -> CALL_ONLY -> IMPORTANT -> ALL -> ...)
  w — push weather    (cycle CLEAR -> PARTLY_CLOUDY -> CLOUDY -> RAIN ->
                        THUNDERSTORM -> SNOW -> FOG -> ..., each with a
                        plausible temp — see WEATHER_CYCLE)
  r — read Device Settings (show Ori's current clock_face + time_format + ancs_filter from NVS)
  f — factory reset   (§7.2)
  q — quit
"""

async def command_loop(orion: MockOrion, initial_settings: Optional[dict] = None):
    loop      = asyncio.get_event_loop()
    presence  = [0x00]   # mutable cell so 'p' can cycle across iterations
    combo_idx = [0]       # mutable cell so 'c' can cycle across iterations
    ancs_idx  = [0]       # mutable cell so 'n' can cycle across iterations
    weather_idx = [0]     # mutable cell so 'w' can cycle across iterations
    # Seed clock_face from Ori's actual NVS value (read on connect) so 'k'
    # toggles relative to what the device is actually showing, not a guess.
    clock_face = [int(initial_settings.get("c", 0x00)) if initial_settings else 0x00]
    # Same for time format ('h' toggle), seeded from Ori's actual NVS value.
    time_format = [int(initial_settings.get("h", 0x00)) if initial_settings else 0x00]
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
            await orion.run_sync()
            print(MENU)
        elif cmd == "p":
            presence[0] = (presence[0] + 1) % 4
            await orion.push_presence(presence[0])
        elif cmd == "c":
            slot1, slot2, slot3 = SHORTCUT_COMBOS[combo_idx[0]]
            combo_idx[0] = (combo_idx[0] + 1) % len(SHORTCUT_COMBOS)
            await orion.push_shortcuts(slot1, slot2, slot3)
        elif cmd == "t":
            await orion.push_time_sync()
        elif cmd == "m":
            await orion.push_media()
        elif cmd == "k":
            clock_face[0] = 0x00 if clock_face[0] else 0x01
            await orion.push_clock_face(clock_face[0])
        elif cmd == "h":
            time_format[0] = 0x00 if time_format[0] else 0x01
            await orion.push_time_format(time_format[0])
        elif cmd == "n":
            ancs_idx[0] = (ancs_idx[0] + 1) % 4
            await orion.push_ancs_filter(ancs_idx[0])
        elif cmd == "w":
            condition, temp_c = WEATHER_CYCLE[weather_idx[0]]
            weather_idx[0] = (weather_idx[0] + 1) % len(WEATHER_CYCLE)
            await orion.push_weather(condition, temp_c)
        elif cmd == "r":
            await orion.read_device_settings()
        elif cmd == "f":
            await orion.run_factory_reset()
            # Don't reprint menu — factory reset exits the loop.
        elif cmd == "q":
            print("  Goodbye.")
            break
        elif cmd:
            print("  Unknown command. Type s / p / c / t / m / k / h / n / w / r / f / q")

# ── Entry point ───────────────────────────────────────────────────────────────

async def find_ori(address: Optional[str]) -> Optional[str]:
    if address:
        return address
    print("Scanning for Ori devices (10 s)…")
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

async def check_post_disconnect_flag(address: str, timeout: float = 4.0) -> Optional[int]:
    """Passively scan for Ori's post-disconnect advert and read its mode flag.

    ble-protocol.md §7.1: after a factory reset Ori reboots advertising
    manufacturer-data flag 0x01 SETUP (vs the normal 0x02 RUNTIME a plain
    reboot/drop would show, bond still intact). Reading this requires no
    connection attempt — the "preferred" detection path in the spec, vs.
    discovering it the hard way via a stale-bond encryption failure.
    """
    flag: list[Optional[int]] = [None]
    found = asyncio.Event()

    def _on_adv(device, adv_data):
        if device.address.lower() != address.lower():
            return
        data = adv_data.manufacturer_data.get(MFG_COMPANY_ID)
        if data:
            flag[0] = data[0]
            found.set()

    async with BleakScanner(detection_callback=_on_adv):
        try:
            await asyncio.wait_for(found.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
    return flag[0]

async def report_disconnect_reason(address: str):
    flag = await check_post_disconnect_flag(address)
    if flag == ADV_FLAG_SETUP:
        print("\n  [info] Ori is advertising SETUP (0x01) — looks like a factory reset,")
        print("         not just a reboot/drop.")
        print("  ── To re-pair ────────────────────────────────────────────────")
        print("  1. Windows Settings → Bluetooth & devices → find 'Ori'")
        print("     → '...' → Remove device (your old bond is now stale)")
        print("  2. Re-run this script to pair fresh")
        print("  ──────────────────────────────────────────────────────────────\n")
    elif flag == ADV_FLAG_RUNTIME:
        print("  [info] Ori still advertising RUNTIME (0x02) — bond intact, "
              "should auto-reconnect on its own.\n")
    else:
        print("  [info] Didn't see Ori readvertise within 4 s — "
              "check it's powered and in range.\n")

def on_disconnect(client: BleakClient):
    print("\n  [disconnected] BLE link dropped (Ori may have rebooted).")
    asyncio.create_task(report_disconnect_reason(client.address))

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
        # Read Device Settings on connect to recover the two NVS-persisted fields
        # (clock_face + ancs_filter) so command_loop()'s 'k' toggle starts from
        # Ori's actual state, not a guess. Presence and shortcuts are NOT returned
        # by the read — Orion is their sole source of truth (ble-protocol.md §6.4).
        initial_settings = await orion.read_device_settings()
        # Push presence fresh on every connect via char 000E, outside the sync
        # pipeline. run_sync() pushes shortcuts (also char 000E) right before BEGIN.
        await orion.push_presence(args.presence)
        # Weather is likewise ephemeral — push fresh on every connect so the
        # badge isn't left hidden from a previous disconnect (ble-protocol.md §6.4).
        weather_temp = dict(WEATHER_CYCLE).get(args.weather, 20)
        await orion.push_weather(args.weather, weather_temp)
        # Sync automatically on connect — Ori reports which sections differ and we
        # send only those. Running it immediately also satisfies Ori's first-pair
        # BEGIN handshake deadline. Re-run anytime with 's'.
        await orion.run_sync()

        # Push media metadata (no art) immediately so Ori's Controls screen
        # shows title/artist/position right away. Art arrives a few seconds
        # later once the watcher completes its first full fetch+encode.
        if _WINRT_AVAILABLE:
            quick = await read_now_playing_media(quiet=True, fetch_art=False)
            if quick:
                await orion._send_media(quick)

        watcher_task = asyncio.create_task(media_watcher(orion))
        vol_task     = asyncio.create_task(volume_watcher(orion))
        try:
            await command_loop(orion, initial_settings=initial_settings)
        finally:
            watcher_task.cancel()
            vol_task.cancel()
            for t in (watcher_task, vol_task):
                try:
                    await t
                except asyncio.CancelledError:
                    pass

if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    parser = argparse.ArgumentParser(
        description="Mock Orion BLE — interactive Ori tester",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--address", metavar="AA:BB:CC:DD:EE:FF",
                        help="BLE address of Ori (skips scan)")
    parser.add_argument("--photo-quality", type=int, metavar="1-95",
                        help="force JPEG quality for the profile + Time Off photos "
                             "(lower = smaller/worse). Default: highest that fits the cap.")
    parser.add_argument("--presence", type=int, default=0x00, choices=[0x00, 0x01, 0x02, 0x03],
                        help="Presence Status to push on connect: "
                             "0=AVAILABLE (default) 1=BUSY 2=AWAY 3=OFFLINE")
    parser.add_argument("--weather", type=int, default=0x00, choices=range(7), metavar="0-6",
                        help="Weather condition to push on connect (paired temp comes from "
                             "WEATHER_CYCLE): 0=CLEAR (default) 1=PARTLY_CLOUDY 2=CLOUDY "
                             "3=RAIN 4=THUNDERSTORM 5=SNOW 6=FOG")
    parser.add_argument("--stress-meetings", action="store_true",
                        help="send 32 max-length meetings (protocol-cap boundary test) "
                             "instead of the default 6-meeting realistic day")
    args = parser.parse_args()
    if args.photo_quality is not None:
        if not 1 <= args.photo_quality <= 95:
            parser.error("--photo-quality must be between 1 and 95")
        _FORCED_JPEG_QUALITY = args.photo_quality
    _STRESS_MEETINGS = args.stress_meetings
    asyncio.run(main(args))
