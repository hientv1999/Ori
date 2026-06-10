#!/usr/bin/env python3
"""
mock_orion_ota.py — Mock Orion USB CDC OTA tester for Ori (ota.md).

Launches an interactive menu of OTA test scenarios and streams firmware (or
deliberately broken transfers) to Ori over the USB-C serial port. Each scenario
drives a specific branch of the firmware's OTA state machine and reports
PASS/FAIL against the expected response.

OTA is a separate channel from BLE, so this tool needs only the serial port —
no BLE bond, no pairing. (BLE sync/setup testing lives in mock_orion_ble.py.)

Requirements:
    pip install pyserial cbor2

Usage:
    python tools/mock_orion_ota.py                 # interactive menu, defaults
    python tools/mock_orion_ota.py --scenario 1    # run one scenario, then exit
    python tools/mock_orion_ota.py --port COM9 other.bin
    python tools/mock_orion_ota.py --list-ports

Scenarios:
    1  Successful OTA           → VALIDATED, device reboots
    2  Broken cable mid-stream  → stop sending; firmware stall-aborts (usb_timeout)
    3  Wrong version            → FAILED  { version_mismatch } (claim ≠ binary)
    4  Image too large          → REJECT { too_large }
    5  Corrupted image          → FAILED  { hash_mismatch }
    6  Truncated transfer       → FAILED  { truncated }
    7  Oversized stream         → FAILED  { size_overflow }
    8  Double BEGIN while busy  → REJECT  { busy }, original transfer completes
    9  Malformed BEGIN          → REJECT  { cbor_decode / not_map / missing_fields }

Notes:
    • The "good" BEGIN sends the version read from the binary's OriFwVer= marker,
      so the claim matches and the device installs (downgrades/re-installs are OK).
    • After a successful OTA the device reboots and the serial port
      re-enumerates; wait a few seconds before the next scenario.
    • Physical cable access is the authority (ota.md) — no BLE bond required.
    • Close any other serial monitor (PlatformIO / IDE) first; the port is
      exclusive.
"""

import argparse
import hashlib
import sys
import time
from typing import Optional

import cbor2

# ── Defaults (this bench) ─────────────────────────────────────────────────────

DEFAULT_PORT  = "COM72"  # this bench's Ori; override with --port
DEFAULT_FIRMWARE = (
    r"C:\Users\xander.to\Documents\PlatformIO\Projects\Ori"
    r"\firmware\.pio\build\ori\firmware.bin"
)  # default build output; pass a path to override

# Must match ORI_FW_VERSION in firmware/include/fw_version.h — used by the
# "wrong version" scenario to trigger REJECT{already_current}.
RUNNING_FW_VERSION = "1.0.0"
ESPRESSIF_VID      = 0x303A   # serial-port autodetect

# ── Framing (matches firmware src/ota_receiver.cpp) ──────────────────────────
#   Offset  Size  Field
#   0       2     magic       = 0x4F 0x54 ("OT")
#   2       1     op
#   3       3     payload_len (uint24, little-endian)
#   6       N     payload     (CBOR for control ops; raw bytes for DATA)

OTA_MAGIC = b"\x4F\x54"

OTA_OP_BEGIN     = 0x01   # PC → Ori   CBOR { fw_version, total_size, sha256 }
OTA_OP_READY     = 0x02   # Ori → PC   CBOR {}
OTA_OP_REJECT    = 0x03   # Ori → PC   CBOR { reason }
OTA_OP_DATA      = 0x04   # PC → Ori   raw bytes
OTA_OP_PROGRESS  = 0x05   # Ori → PC   CBOR { bytes_received }
OTA_OP_END       = 0x06   # PC → Ori   CBOR {}
OTA_OP_VALIDATED = 0x07   # Ori → PC   CBOR {}
OTA_OP_FAILED    = 0x08   # Ori → PC   CBOR { reason }

OTA_OP_NAME = {
    OTA_OP_BEGIN: "BEGIN", OTA_OP_READY: "READY", OTA_OP_REJECT: "REJECT",
    OTA_OP_DATA: "DATA", OTA_OP_PROGRESS: "PROGRESS", OTA_OP_END: "END",
    OTA_OP_VALIDATED: "VALIDATED", OTA_OP_FAILED: "FAILED",
}

# Frame demux robustness: the 2-byte magic "OT" (0x4F54) also occurs inside the
# firmware's own log text — e.g. "[gatt] OTA active: 1" contains "OT". A naive
# "magic = frame start" demux latches onto that and mis-reads a bogus op/length,
# swallowing the real frame that follows. So after seeing the magic we VALIDATE:
# the op must be a known Ori→PC code and the payload length must be sane. All
# device→PC frames are tiny (READY/VALIDATED ~1 B, REJECT/FAILED/PROGRESS small
# CBOR), so anything larger is a false magic inside log bytes → emit as log and
# resync. (The real Orion central must apply the same validation.)
VALID_RESP_OPS    = {OTA_OP_READY, OTA_OP_REJECT, OTA_OP_PROGRESS,
                     OTA_OP_VALIDATED, OTA_OP_FAILED}
MAX_RESP_PAYLOAD  = 4096

# Windowed flow control: max bytes the sender may have unacked (sent − acked).
# Must be < the device's Serial RX buffer (32 KB) and > its PROGRESS ack
# interval (8 KB) so it neither overflows nor deadlocks.
OTA_WINDOW        = 16384


def ota_frame(op: int, payload: bytes = b"") -> bytes:
    return OTA_MAGIC + bytes([op & 0xFF]) + len(payload).to_bytes(3, "little") + payload


# ── Frame reader: demuxes framed responses from interleaved boot-log text ────

class SerialFrameReader:
    """next_event() returns ("frame", op, payload) | ("log", raw) | None."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def _pump_available(self) -> bool:
        n = self.ser.in_waiting
        if n:
            self.buf.extend(self.ser.read(n))
            return True
        return False

    def _pump_blocking(self):
        chunk = self.ser.read(1)          # honours the port read timeout
        if chunk:
            self.buf.extend(chunk)
            self._pump_available()        # batch in whatever else landed

    def _extract(self):
        b = self.buf
        if not b:
            return None
        idx = b.find(OTA_MAGIC)
        if idx < 0:
            # Hold back a trailing lone 0x4F (possible first half of a magic).
            if b[-1:] == b"\x4F":
                text = bytes(b[:-1]); del b[:-1]
            else:
                text = bytes(b); b.clear()
            return ("log", text) if text else None
        if idx > 0:
            text = bytes(b[:idx]); del b[:idx]
            return ("log", text)
        if len(b) < 6:
            return None                      # header incomplete — wait for more
        op   = b[2]
        plen = int.from_bytes(b[3:6], "little")
        if op not in VALID_RESP_OPS or plen > MAX_RESP_PAYLOAD:
            # False magic inside log text (e.g. the "OT" in "OTA active"). Emit
            # the first magic byte as log and resync past it.
            text = bytes(b[:1]); del b[:1]
            return ("log", text)
        if len(b) < 6 + plen:
            return None                      # payload incomplete — wait for more
        payload = bytes(b[6:6 + plen]); del b[:6 + plen]
        return ("frame", op, payload)

    def next_event(self, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            ev = self._extract()
            if ev:
                return ev
            if time.monotonic() >= deadline:
                return None
            self._pump_blocking()

    def drain(self, on_frame):
        """Process everything currently buffered/available without blocking."""
        self._pump_available()
        while True:
            ev = self._extract()
            if not ev:
                return
            if ev[0] == "log":
                _emit_device_log(ev[1])
            else:
                on_frame(ev[1], ev[2])

    def wait_frame(self, timeout: float, want: set, on_frame=None):
        """Block until a frame whose op is in `want` arrives, or timeout (None)."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            ev = self.next_event(left)
            if ev is None:
                return None
            if ev[0] == "log":
                _emit_device_log(ev[1])
                continue
            _, op, payload = ev
            if op in want:
                return (op, payload)
            if on_frame:
                on_frame(op, payload)
            else:
                _print_other_frame(op, payload)


# ── Output helpers ───────────────────────────────────────────────────────────

_log_partial = ""

def _emit_device_log(raw: bytes):
    global _log_partial
    text = _log_partial + raw.decode("utf-8", "replace")
    parts = text.split("\n")
    _log_partial = parts[-1]
    for line in parts[:-1]:
        line = line.rstrip("\r")
        if line:
            print(f"  \033[2m[ori] {line}\033[0m")

def _flush_device_log():
    global _log_partial
    if _log_partial.strip():
        print(f"  \033[2m[ori] {_log_partial.rstrip()}\033[0m")
    _log_partial = ""

def _decode_reason(payload: bytes) -> str:
    try:
        msg = cbor2.loads(payload)
        if isinstance(msg, dict):
            return msg.get("reason", repr(msg))
        return repr(msg)
    except Exception:
        return payload.hex()

def _print_other_frame(op: int, payload: bytes):
    name = OTA_OP_NAME.get(op, f"0x{op:02X}")
    if op == OTA_OP_PROGRESS:
        try:
            n = cbor2.loads(payload).get("bytes_received")
            print(f"  [ori] PROGRESS bytes_received={n}")
        except Exception:
            print(f"  [ori] PROGRESS {payload.hex()}")
    else:
        print(f"  [ori] {name} {payload.hex()}")

def _verdict(passed: bool, expected: str, actual: str) -> bool:
    mark = "\033[32mPASS\033[0m" if passed else "\033[31mFAIL\033[0m"
    print(f"\n  ── RESULT: {mark}")
    print(f"     expected: {expected}")
    print(f"     actual:   {actual}\n")
    return passed


# ── Serial port discovery ─────────────────────────────────────────────────────

def autodetect_port() -> Optional[str]:
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    cands = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if len(cands) == 1:
        print(f"  Autodetected Ori serial port: {cands[0].device} "
              f"({cands[0].description})")
        return cands[0].device
    if not cands:
        print("  No Espressif USB serial port found.")
    else:
        print("  Multiple Espressif serial ports — specify one with --port:")
        for p in cands:
            print(f"    {p.device}  {p.description}")
    return None


def print_serial_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial")
        return
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Serial ports:")
    for p in ports:
        vid = f"{p.vid:04X}" if p.vid is not None else "----"
        pid = f"{p.pid:04X}" if p.pid is not None else "----"
        tag = "  <- likely Ori" if p.vid == ESPRESSIF_VID else ""
        print(f"  {p.device:8}  VID:PID {vid}:{pid}  {p.description}{tag}")


# ── OTA session ───────────────────────────────────────────────────────────────

class OtaSession:
    """Thin wrapper around an open serial port + frame reader."""

    def __init__(self, ser):
        self.ser    = ser
        self.reader = SerialFrameReader(ser)

    boot_bytes = 0

    def flush_boot_logs(self):
        # Read briefly and report how chatty the device is. Zero bytes here is
        # the tell-tale of a wrong port / DTR-suppressed TX / firmware not running.
        time.sleep(0.2)
        seen = 0
        t_end = time.monotonic() + 0.5
        while time.monotonic() < t_end:
            seen += self.ser.in_waiting
            self.reader.drain(lambda op, pl: _print_other_frame(op, pl))
            time.sleep(0.05)
        self.boot_bytes = seen
        if seen:
            print(f"  [diag] device emitted {seen} byte(s) since open — link looks alive")
        else:
            print("  [diag] device emitted 0 bytes since open — no data on this port "
                  "(check port / firmware / DTR)")

    def send(self, op: int, payload: bytes = b""):
        self.ser.write(ota_frame(op, payload))
        self.ser.flush()

    def begin(self, fw_version: str, total_size: int, digest: bytes, raw=None):
        if raw is None:
            raw = cbor2.dumps({"fw_version": fw_version,
                               "total_size": total_size,
                               "sha256":     digest})
        self.send(OTA_OP_BEGIN, raw)

    def wait(self, want: set, timeout: float, on_frame=None):
        return self.reader.wait_frame(timeout, want, on_frame)

    def stream(self, data: bytes, chunk: int, on_frame, state: dict,
               window: int = OTA_WINDOW, stop=None) -> int:
        """Send `data` as DATA frames with windowed flow control.

        USB CDC has no reliable app-level back-pressure on the ESP32-S3 HWCDC,
        and the device is flash-write bound (it cannot sink the bytes at USB
        line rate). So we never let the unacked window (sent − acked) exceed
        `window`, which must be < the device RX buffer and > the PROGRESS ack
        interval. `state["acked"]` is updated from PROGRESS frames by the
        handler. Also drains responses between frames — the firmware does a
        blocking Serial.flush() after each PROGRESS, so we must keep reading.
        `stop()` lets a scenario bail when the device aborts (FAILED).
        """
        sent = 0
        for i in range(0, len(data), chunk):
            if stop and stop():
                break
            # Flow control: wait for acks to catch up before overrunning.
            wait_start = time.monotonic()
            while sent - state.get("acked", 0) >= window:
                self.reader.drain(on_frame)
                if stop and stop():
                    return sent
                if time.monotonic() - wait_start > 10.0:
                    return sent          # device stopped acking — let caller time out
                time.sleep(0.001)
            self.ser.write(ota_frame(OTA_OP_DATA, data[i:i + chunk]))
            sent += len(data[i:i + chunk])
            self.reader.drain(on_frame)
        self.ser.flush()
        return sent

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


def open_session(args) -> Optional[OtaSession]:
    try:
        import serial
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial")
        return None
    port = args.port or autodetect_port()
    if not port:
        print("[error] no serial port. Use --port COMx (or --list-ports).")
        return None
    try:
        ser = serial.Serial()
        ser.port     = port
        ser.baudrate = args.baud
        ser.timeout  = 0.1
        # The ESP32-S3 native USB-Serial-JTAG (HWCDC) only transmits once the
        # host asserts DTR — with DTR low the device's READY and LOG() output
        # are suppressed and the tool just times out. Assert DTR; leave RTS low.
        # A steady DTR level does not reset the chip (only the esptool DTR/RTS
        # toggle *sequence* does), so this is safe.
        ser.dtr      = args.dtr
        ser.rts      = False
        ser.open()
    except serial.SerialException as exc:
        print(f"[error] cannot open {port}: {exc}")
        print("        If Ori just rebooted it may still be re-enumerating — "
              "wait a few seconds and retry.")
        print("        Also close any other serial monitor (PlatformIO/IDE).")
        return None
    s = OtaSession(ser)
    s.flush_boot_logs()
    return s


def _stream_handler(total: int, state: dict):
    """Frame handler used during DATA streaming.

    FAILED is terminal (sets stop). REJECT is recorded but non-terminal (a
    second BEGIN while busy is rejected without ending the running transfer).
    """
    def on_frame(op, payload):
        if op == OTA_OP_PROGRESS:
            try:
                n = cbor2.loads(payload).get("bytes_received", 0)
                state["acked"] = n          # drives the sender's flow control
                # Throttle console output to every ~10% — printing every 8 KB ack
                # to a Windows console is slow enough to throttle the send loop.
                pct = 100 * n // total
                if pct >= state.get("_next_print", 0):
                    print(f"  [ori] {pct:3d}%  ({n}/{total} bytes acked)")
                    state["_next_print"] = pct + 10
            except Exception:
                pass
        elif op == OTA_OP_FAILED:
            state["failed"] = _decode_reason(payload); state["stop"] = True
        elif op == OTA_OP_REJECT:
            state.setdefault("rejects", []).append(_decode_reason(payload))
            print(f"  [ori] REJECT {{ {state['rejects'][-1]} }}")
    return on_frame


# ══════════════════════════════════════════════════════════════════════════════
#  Scenarios — each returns True (pass) / False (fail)
# ══════════════════════════════════════════════════════════════════════════════

def _begin_and_expect_ready(s: OtaSession, fw_version, total_size, digest, raw=None):
    print("  waiting for READY (up to 10 s)…")
    s.begin(fw_version, total_size, digest, raw=raw)
    fr = s.wait({OTA_OP_READY, OTA_OP_REJECT, OTA_OP_FAILED}, 10.0)
    if fr is None:
        print("  [diag] no response to BEGIN. Likely causes:")
        if not s.boot_bytes:
            print("    • device sent 0 bytes — wrong COM port, or DTR not asserted")
            print("      (this build needs DTR; default is on — see --dtr/--no-dtr),")
            print("      or the firmware isn't running. Try menu 'l' to list ports,")
            print("      or 'm' to monitor raw device output.")
        else:
            print("    • device is talking but didn't answer BEGIN — is it running")
            print("      OTA-capable firmware (this build)? Is a 5-min countdown up?")
        print("    • another serial monitor (PlatformIO/IDE) may hold the port.")
    return fr


def scenario_success(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        print("── BEGIN → READY ──")
        fr = _begin_and_expect_ready(s, args.fw_version, len(image), digest)
        if not fr or fr[0] != OTA_OP_READY:
            actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]} {{ {_decode_reason(fr[1])} }}"
            return _verdict(False, "READY", actual)
        print("  [ok] READY — streaming\n")

        state = {}
        t0 = time.monotonic()
        s.stream(image, args.chunk, _stream_handler(len(image), state), state,
                 stop=lambda: state.get("stop"))
        if state.get("stop"):
            return _verdict(False, "VALIDATED", f"FAILED {{ {state['failed']} }}")

        print("\n── END → VALIDATED ──")
        s.send(OTA_OP_END)
        print("  Image downloaded — Ori auto-installs (a few seconds on the")
        print("  'Installing firmware' screen, then the flash commit + reboot)…")
        # Install is automatic: END → Installing linger (~3.5 s) → flash commit →
        # VALIDATED → reboot. No user interaction.
        fr = s.wait({OTA_OP_VALIDATED, OTA_OP_FAILED, OTA_OP_REJECT}, 45.0,
                    _stream_handler(len(image), state))
        dt = max(time.monotonic() - t0, 1e-6)
        if fr and fr[0] == OTA_OP_VALIDATED:
            print(f"  [ok] VALIDATED in {dt:.1f}s "
                  f"({len(image)/1024/dt:.0f} KB/s) — Ori is rebooting.")
            time.sleep(1.5)
            s.reader.drain(lambda o, p: None)
            return _verdict(True, "VALIDATED", "VALIDATED")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]} {{ {_decode_reason(fr[1])} }}"
        return _verdict(False, "VALIDATED", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_broken_cable(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        print("── BEGIN → READY ──")
        fr = _begin_and_expect_ready(s, args.fw_version, len(image), digest)
        if not fr or fr[0] != OTA_OP_READY:
            return _verdict(False, "READY", "no READY")
        print("  [ok] READY — streaming ~30% then 'cutting the cable'\n")

        # Send roughly the first third, then stop sending entirely.
        cut = max(args.chunk, len(image) // 3)
        state = {}
        s.stream(image[:cut], args.chunk, _stream_handler(len(image), state), state)
        print(f"\n  [test] stopped after {min(cut, len(image))} bytes — no more "
              "DATA. Firmware stall watchdog should abort in ~10 s.\n")

        # Watch for the firmware's stall-abort (ota_abort → FAILED{usb_timeout}).
        fr = s.wait({OTA_OP_FAILED, OTA_OP_REJECT}, 15.0)
        if fr and fr[0] == OTA_OP_FAILED:
            reason = _decode_reason(fr[1])
            return _verdict(reason == "usb_timeout",
                            "FAILED { usb_timeout }", f"FAILED {{ {reason} }}")
        return _verdict(False, "FAILED { usb_timeout }",
                        "timeout — no abort frame (did the watchdog fire?)")
    finally:
        _flush_device_log()
        s.close()


def scenario_wrong_version(image, digest, args) -> bool:
    # Orion claims a version that doesn't match the one stamped inside the binary.
    # The firmware downloads, then fails at END with version_mismatch.
    s = open_session(args)
    if not s:
        return False
    try:
        bogus = "9.9.9"   # deliberately != the binary's embedded version
        print(f"── BEGIN fw_version='{bogus}' (≠ the version inside the binary) ──")
        fr = _begin_and_expect_ready(s, bogus, len(image), digest)
        if not (fr and fr[0] == OTA_OP_READY):
            actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]} {{ {_decode_reason(fr[1])} }}"
            return _verdict(False, "READY then FAILED { version_mismatch }", actual)
        state = {"acked": 0}
        s.stream(image, args.chunk, _stream_handler(len(image), state), state)
        s.send(OTA_OP_END)
        fr = s.wait({OTA_OP_VALIDATED, OTA_OP_FAILED}, 10.0,
                    _stream_handler(len(image), state))
        reason = _decode_reason(fr[1]) if fr and fr[1] else ""
        ok = bool(fr and fr[0] == OTA_OP_FAILED and reason == "version_mismatch")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]} {{ {reason} }}"
        return _verdict(ok, "FAILED { version_mismatch }", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_too_large(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        bogus = 4 * 1024 * 1024  # > 3 MB OTA slot
        print(f"── BEGIN with total_size={bogus} (> 3 MB slot) ──")
        fr = _begin_and_expect_ready(s, args.fw_version, bogus, digest)
        if fr and fr[0] == OTA_OP_REJECT:
            reason = _decode_reason(fr[1])
            return _verdict(reason == "too_large",
                            "REJECT { too_large }", f"REJECT {{ {reason} }}")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]}"
        return _verdict(False, "REJECT { too_large }", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_corrupted(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        bad = bytearray(digest); bad[-1] ^= 0xFF   # wrong SHA-256
        print("── BEGIN with corrupted sha256, stream full image ──")
        fr = _begin_and_expect_ready(s, args.fw_version, len(image), bytes(bad))
        if not fr or fr[0] != OTA_OP_READY:
            return _verdict(False, "READY", "no READY")
        print("  [ok] READY — streaming real bytes (hash will mismatch)\n")
        state = {}
        s.stream(image, args.chunk, _stream_handler(len(image), state), state,
                 stop=lambda: state.get("stop"))
        s.send(OTA_OP_END)
        fr = s.wait({OTA_OP_VALIDATED, OTA_OP_FAILED}, 45.0,
                    _stream_handler(len(image), state))
        if fr and fr[0] == OTA_OP_FAILED:
            reason = _decode_reason(fr[1])
            return _verdict(reason == "hash_mismatch",
                            "FAILED { hash_mismatch }", f"FAILED {{ {reason} }}")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]}"
        return _verdict(False, "FAILED { hash_mismatch }", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_truncated(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        print("── BEGIN (correct), stream ~50%, then END early ──")
        fr = _begin_and_expect_ready(s, args.fw_version, len(image), digest)
        if not fr or fr[0] != OTA_OP_READY:
            return _verdict(False, "READY", "no READY")
        print("  [ok] READY — streaming half then END\n")
        half = max(1, len(image) // 2)   # strictly < total so END truncates
        state = {}
        s.stream(image[:half], args.chunk, _stream_handler(len(image), state), state)
        s.send(OTA_OP_END)   # received != total → firmware aborts 'truncated'
        fr = s.wait({OTA_OP_VALIDATED, OTA_OP_FAILED}, 15.0,
                    _stream_handler(len(image), state))
        if fr and fr[0] == OTA_OP_FAILED:
            reason = _decode_reason(fr[1])
            return _verdict(reason == "truncated",
                            "FAILED { truncated }", f"FAILED {{ {reason} }}")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]}"
        return _verdict(False, "FAILED { truncated }", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_oversized(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        declared = max(1, len(image) - 8192)   # lie: declare fewer bytes
        print(f"── BEGIN total_size={declared} but stream {len(image)} bytes ──")
        fr = _begin_and_expect_ready(s, args.fw_version, declared, digest)
        if not fr or fr[0] != OTA_OP_READY:
            return _verdict(False, "READY", "no READY")
        print("  [ok] READY — overrunning declared size\n")
        state = {}
        s.stream(image, args.chunk, _stream_handler(declared, state), state,
                 stop=lambda: state.get("stop"))
        if state.get("failed"):
            reason = state["failed"]
            return _verdict(reason == "size_overflow",
                            "FAILED { size_overflow }", f"FAILED {{ {reason} }}")
        # Some bytes may still be in flight — give the abort a moment.
        fr = s.wait({OTA_OP_FAILED}, 5.0)
        reason = _decode_reason(fr[1]) if fr else "no abort"
        return _verdict(reason == "size_overflow",
                        "FAILED { size_overflow }", f"{reason}")
    finally:
        _flush_device_log()
        s.close()


def scenario_double_begin(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        print("── BEGIN → READY, stream a bit, then a 2nd BEGIN ──")
        fr = _begin_and_expect_ready(s, args.fw_version, len(image), digest)
        if not fr or fr[0] != OTA_OP_READY:
            return _verdict(False, "READY", "no READY")
        print("  [ok] READY\n")
        state = {}
        handler = _stream_handler(len(image), state)
        chunk = args.chunk

        # Stream the first quarter.
        q = max(chunk, len(image) // 4)
        s.stream(image[:q], chunk, handler, state, stop=lambda: state.get("stop"))

        # Inject a duplicate BEGIN — firmware must REJECT{busy} and keep going.
        print("\n  [test] sending a 2nd BEGIN mid-transfer (expect busy)\n")
        s.begin(args.fw_version, len(image), digest)
        # Give the device a beat to answer, draining responses.
        t_end = time.monotonic() + 1.0
        while time.monotonic() < t_end:
            s.reader.drain(handler)
            time.sleep(0.05)

        # Finish the ORIGINAL transfer.
        s.stream(image[q:], chunk, handler, state, stop=lambda: state.get("stop"))
        s.send(OTA_OP_END)
        fr = s.wait({OTA_OP_VALIDATED, OTA_OP_FAILED}, 45.0, handler)

        got_busy  = "busy" in (state.get("rejects") or [])
        validated = bool(fr and fr[0] == OTA_OP_VALIDATED)
        actual = (f"REJECT busy={got_busy}, "
                  f"final={'VALIDATED' if validated else (OTA_OP_NAME[fr[0]] if fr else 'timeout')}")
        if validated:
            time.sleep(1.5); s.reader.drain(lambda o, p: None)
        return _verdict(got_busy and validated,
                        "REJECT { busy } then VALIDATED", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_malformed_begin(image, digest, args) -> bool:
    s = open_session(args)
    if not s:
        return False
    try:
        print("── BEGIN with non-CBOR-map payload ──")
        fr = _begin_and_expect_ready(s, "", 0, b"", raw=b"\xff\x00not-cbor-map")
        if fr and fr[0] == OTA_OP_REJECT:
            reason = _decode_reason(fr[1])
            ok = reason in ("cbor_decode", "not_map", "missing_fields")
            return _verdict(ok, "REJECT { cbor_decode / not_map / missing_fields }",
                            f"REJECT {{ {reason} }}")
        actual = "timeout" if not fr else f"{OTA_OP_NAME[fr[0]]}"
        return _verdict(False, "REJECT { cbor_decode / ... }", actual)
    finally:
        _flush_device_log()
        s.close()


def scenario_monitor(image, digest, args) -> bool:
    """Not an OTA test — just dump whatever the device sends, to confirm the
    link/port before running a real scenario."""
    s = open_session(args)
    if not s:
        return False
    print("  [monitor] dumping device output for 8 s (Ctrl-C to stop early)…\n")
    try:
        t_end = time.monotonic() + 8.0
        while time.monotonic() < t_end:
            s.reader.drain(lambda op, pl: _print_other_frame(op, pl))
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        _flush_device_log()
        s.close()
    return True


SCENARIOS = [
    ("1", "Successful OTA",          scenario_success),
    ("2", "Broken cable mid-stream", scenario_broken_cable),
    ("3", "Wrong version",           scenario_wrong_version),
    ("4", "Image too large",         scenario_too_large),
    ("5", "Corrupted image",         scenario_corrupted),
    ("6", "Truncated transfer",      scenario_truncated),
    ("7", "Oversized stream",        scenario_oversized),
    ("8", "Double BEGIN (busy)",     scenario_double_begin),
    ("9", "Malformed BEGIN",         scenario_malformed_begin),
]
SCENARIO_MAP = {key: fn for key, _, fn in SCENARIOS}

EXPECTED = {
    "1": "VALIDATED (device reboots)",
    "2": "FAILED { usb_timeout } after ~10 s",
    "3": "REJECT { already_current }",
    "4": "REJECT { too_large }",
    "5": "FAILED { hash_mismatch }",
    "6": "FAILED { truncated }",
    "7": "FAILED { size_overflow }",
    "8": "REJECT { busy }, then VALIDATED",
    "9": "REJECT { cbor_decode / not_map / missing_fields }",
}


# ── Menu ──────────────────────────────────────────────────────────────────────

def print_menu(args):
    print("\n╔══════════════════════════════════════════════════════════════╗")
    print("║  Ori USB CDC OTA tester                                        ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    print(f"  firmware : {args.firmware}")
    print(f"  port     : {args.port}    fw_version (good): {args.fw_version}")
    print("  ──────────────────────────────────────────────────────────────")
    for key, title, _ in SCENARIOS:
        print(f"   {key}  {title:<24}→ expect {EXPECTED[key]}")
    print("   m  Monitor raw device output (confirm port/link)")
    print("   l  List serial ports")
    print("   q  Quit")


def menu_loop(image, digest, args):
    while True:
        print_menu(args)
        try:
            choice = input("\n  select> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if choice in ("q", "quit", "exit"):
            return
        if choice in ("l", "list"):
            print_serial_ports()
            continue
        if choice in ("m", "monitor"):
            scenario_monitor(image, digest, args)
            input("  press Enter to return to the menu… ")
            continue
        fn = SCENARIO_MAP.get(choice)
        if not fn:
            print("  unknown selection.")
            continue
        title = next(t for k, t, _ in SCENARIOS if k == choice)
        print(f"\n══════════ Scenario {choice}: {title} ══════════")
        try:
            fn(image, digest, args)
        except KeyboardInterrupt:
            print("\n  [interrupted] scenario aborted.")
        except Exception as exc:
            print(f"  [error] scenario raised: {exc!r}")
        input("  press Enter to return to the menu… ")


# ── Entry point ───────────────────────────────────────────────────────────────

def extract_fw_version(image: bytes):
    """Read the version stamped inside the image (the "OriFwVer=<ver>" marker),
    the same value the firmware checks the BEGIN claim against. Returns None if
    absent (pre-marker image)."""
    i = image.find(b"OriFwVer=")
    if i < 0:
        return None
    end = image.find(b"\x00", i + 9)
    if end < 0:
        return None
    return image[i + 9:end].decode("ascii", "replace")


def load_image(path: str):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        print(f"[error] cannot read firmware '{path}': {exc}")
        return None, None
    if not data:
        print("[error] firmware file is empty.")
        return None, None
    digest = hashlib.sha256(data).digest()
    print(f"  image: {path}\n  size:  {len(data)} bytes ({len(data)/1024:.1f} KB)"
          f"\n  sha256: {digest.hex()}")
    return data, digest


if __name__ == "__main__":
    # The menu/output uses box-drawing + arrow glyphs; force UTF-8 so the
    # default Windows console (cp1252) doesn't raise UnicodeEncodeError.
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        description="Mock Orion — USB CDC OTA scenario tester for Ori",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("firmware", nargs="?", metavar="FIRMWARE.bin",
                        default=DEFAULT_FIRMWARE,
                        help="firmware image to stream (default: the ori build output)")
    parser.add_argument("--port", metavar="COMx", default=DEFAULT_PORT,
                        help=f"serial port (default {DEFAULT_PORT}; pass empty "
                             "to autodetect)")
    parser.add_argument("--scenario", metavar="N", choices=SCENARIO_MAP,
                        help="run one scenario non-interactively, then exit")
    parser.add_argument("--fw-version", default="1.0.0",
                        help="fw_version sent in the 'good' BEGIN (default 1.0.0)")
    parser.add_argument("--chunk", type=int, default=4096,
                        help="DATA frame payload size in bytes (default 4096)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="serial baud (USB CDC ignores it; default 115200)")
    parser.add_argument("--dtr", dest="dtr", action="store_true", default=True,
                        help="assert DTR on open (default; HWCDC needs it to TX)")
    parser.add_argument("--no-dtr", dest="dtr", action="store_false",
                        help="leave DTR deasserted (rarely needed)")
    parser.add_argument("--list-ports", action="store_true",
                        help="list serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        print_serial_ports()
        sys.exit(0)

    image, digest = load_image(args.firmware)
    if image is None:
        sys.exit(2)

    # The version check compares the BEGIN claim against the version inside the
    # binary, so the "good" BEGIN must carry the binary's own version. Read it
    # from the image (like Orion would) and use it unless the user overrode it.
    binver = extract_fw_version(image)
    if binver:
        print(f"  embedded version (OriFwVer): {binver}")
        if args.fw_version == "1.0.0":   # the default — replace with the real one
            args.fw_version = binver

    if args.scenario:
        ok = SCENARIO_MAP[args.scenario](image, digest, args)
        sys.exit(0 if ok else 1)

    menu_loop(image, digest, args)
