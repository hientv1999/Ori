//! USB CDC firmware-update sender (ota.md) — the Orion (PC) side of the
//! BEGIN/DATA/END framed protocol Ori's `ota_receiver.cpp` implements.
//! Runs entirely off BLE (`ota.md`: "physical cable access is sufficient
//! authority"); the only input is a firmware `.bin` on disk, picked in
//! `commands::firmware_install`.
//!
//! Mirrors `tools/mock_orion_ota.py`'s reference implementation (frame
//! demux, windowed flow control, timeouts) — that script is the executable
//! spec for this wire behavior.

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::io::{ErrorKind, Write};
use std::path::Path;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};

const ESPRESSIF_VID: u16 = 0x303A;

const OP_BEGIN: u8 = 0x01;
const OP_READY: u8 = 0x02;
const OP_REJECT: u8 = 0x03;
const OP_DATA: u8 = 0x04;
const OP_PROGRESS: u8 = 0x05;
const OP_END: u8 = 0x06;
const OP_VALIDATED: u8 = 0x07;
const OP_FAILED: u8 = 0x08;

/// Every device→PC response frame is small — used to reject a false-positive
/// magic match inside the firmware's own boot-log text (§ "Frame reader:
/// robust to log noise", ota.md).
const VALID_RESP_OPS: [u8; 5] = [OP_READY, OP_REJECT, OP_PROGRESS, OP_VALIDATED, OP_FAILED];
const MAX_RESP_PAYLOAD: usize = 4096;

/// Max unacked bytes (sent − acked) in flight — must stay under Ori's 32 KB
/// Serial RX buffer and above its ~8 KB PROGRESS ack interval (ota.md §5).
const WINDOW: u64 = 16384;
const CHUNK: usize = 4096;

const BEGIN_TIMEOUT: Duration = Duration::from_secs(10);
const STREAM_STALL_TIMEOUT: Duration = Duration::from_secs(10);
const END_TIMEOUT: Duration = Duration::from_secs(45);

#[derive(Serialize, Clone)]
struct FwProgress {
    pct: f32,
    phase: &'static str,
    version: Option<String>,
    reason: Option<String>,
}

fn emit(app: &AppHandle, pct: f32, phase: &'static str, version: Option<String>, reason: Option<String>) {
    let _ = app.emit("fw-progress", FwProgress { pct, phase, version, reason });
}

/// Entry point — called from `commands::firmware_install` inside
/// `spawn_blocking` (this function does blocking serial I/O throughout, so
/// it must never run on the async reactor). Every failure path funnels
/// through here so the frontend always gets a terminal `fw-progress` event
/// (phase `"done"` or `"failed"`) — never leaves the modal's progress ring
/// spinning forever.
pub fn run_update(app: &AppHandle, firmware_path: &Path) {
    if let Err(reason) = run_update_inner(app, firmware_path) {
        emit(app, 0.0, "failed", None, Some(reason));
    }
}

fn run_update_inner(app: &AppHandle, firmware_path: &Path) -> Result<(), String> {
    let image = std::fs::read(firmware_path).map_err(|e| format!("Couldn't read the firmware file: {e}"))?;
    if image.is_empty() {
        return Err("Firmware file is empty.".to_string());
    }
    if image.first() != Some(&0xE9) {
        return Err("This doesn't look like a valid Ori firmware file (bad image header).".to_string());
    }
    let fw_version = extract_fw_version(&image)
        .ok_or_else(|| "This doesn't look like a valid Ori firmware file (no version marker).".to_string())?;
    let total_size = image.len() as u64;
    let digest: [u8; 32] = Sha256::digest(&image).into();

    emit(app, 0.0, "downloading", None, None);

    let port_name = find_ori_port()?;
    let mut port = serialport::new(&port_name, 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|e| format!("Couldn't open {port_name}: {e}"))?;
    // The ESP32-S3 native USB-Serial-JTAG (HWCDC) only transmits once the
    // host asserts DTR — READY/PROGRESS/etc. are suppressed with DTR low. A
    // steady DTR level doesn't reset the chip (only esptool's DTR/RTS toggle
    // *sequence* does), so this is safe to leave asserted for the session.
    port.write_data_terminal_ready(true).map_err(|e| e.to_string())?;
    port.write_request_to_send(false).map_err(|e| e.to_string())?;

    let mut reader = FrameReader::new();

    // ── BEGIN → READY ───────────────────────────────────────────────────
    port.write_all(&ota_frame(OP_BEGIN, &cbor_begin(&fw_version, total_size, &digest)))
        .map_err(|e| e.to_string())?;
    port.flush().ok();
    match wait_frame(&mut *port, &mut reader, &[OP_READY, OP_REJECT, OP_FAILED], BEGIN_TIMEOUT)? {
        (OP_READY, _) => {}
        (OP_REJECT, payload) | (OP_FAILED, payload) => return Err(friendly_reason(&decode_reason(&payload))),
        _ => unreachable!(),
    }

    // ── DATA, windowed flow control ─────────────────────────────────────
    let mut sent: u64 = 0;
    let mut acked: u64 = 0;
    let mut next_report_pct = 0.0f32;
    for chunk in image.chunks(CHUNK) {
        let wait_start = Instant::now();
        while sent.saturating_sub(acked) >= WINDOW {
            if let Some((op, payload)) = try_read_frame(&mut *port, &mut reader)? {
                match op {
                    OP_PROGRESS => acked = decode_progress(&payload),
                    OP_FAILED => return Err(friendly_reason(&decode_reason(&payload))),
                    _ => {}
                }
            } else {
                std::thread::sleep(Duration::from_millis(2));
            }
            if wait_start.elapsed() > STREAM_STALL_TIMEOUT {
                return Err("Ori stopped acknowledging data — check the USB cable and try again.".to_string());
            }
        }
        port.write_all(&ota_frame(OP_DATA, chunk)).map_err(|e| e.to_string())?;
        sent += chunk.len() as u64;
        while let Some((op, payload)) = try_read_frame(&mut *port, &mut reader)? {
            match op {
                OP_PROGRESS => {
                    acked = decode_progress(&payload);
                    let pct = 90.0 * (acked as f32 / total_size as f32).min(1.0);
                    if pct >= next_report_pct {
                        emit(app, pct, "downloading", None, None);
                        next_report_pct = pct + 2.0;
                    }
                }
                OP_FAILED => return Err(friendly_reason(&decode_reason(&payload))),
                _ => {}
            }
        }
    }
    port.flush().ok();

    // ── END → automatic install (verify → install → flash commit) ──────
    // No PROGRESS-equivalent exists between END and the terminal response —
    // firmware runs the SHA-256 check, version check, ~3.5s "Installing"
    // linger, then the flash commit, entirely on its own. Approximate the
    // sub-phases with elapsed time purely for the progress ring; the only
    // real signal is the terminal VALIDATED/FAILED frame.
    emit(app, 92.0, "verifying", None, None);
    port.write_all(&ota_frame(OP_END, &CBOR_EMPTY_MAP)).map_err(|e| e.to_string())?;
    port.flush().ok();

    let deadline = Instant::now() + END_TIMEOUT;
    let mut announced_installing = false;
    loop {
        if let Some((op, payload)) = reader.extract() {
            match op {
                OP_VALIDATED => break,
                OP_FAILED | OP_REJECT => return Err(friendly_reason(&decode_reason(&payload))),
                _ => continue,
            }
        }
        if !announced_installing && Instant::now() >= deadline - END_TIMEOUT + Duration::from_secs(4) {
            emit(app, 96.0, "installing", None, None);
            announced_installing = true;
        }
        if Instant::now() >= deadline {
            return Err("Timed out waiting for Ori to finish installing.".to_string());
        }
        reader.pump_blocking(&mut *port)?;
    }

    emit(app, 100.0, "done", Some(fw_version), None);
    Ok(())
}

// ── Serial port discovery ───────────────────────────────────────────────

fn find_ori_port() -> Result<String, String> {
    let ports = serialport::available_ports().map_err(|e| e.to_string())?;
    let candidates: Vec<String> = ports
        .into_iter()
        .filter(|p| matches!(&p.port_type, serialport::SerialPortType::UsbPort(info) if info.vid == ESPRESSIF_VID))
        .map(|p| p.port_name)
        .collect();
    match candidates.len() {
        0 => Err("Ori not found — plug it into this PC via USB, then try again.".to_string()),
        1 => Ok(candidates.into_iter().next().unwrap()),
        _ => Err(format!(
            "Multiple Ori-like devices found ({}) — unplug the others and try again.",
            candidates.join(", ")
        )),
    }
}

// ── Framing (matches firmware src/ota_receiver.cpp + ota.md § Framing) ───
//   Offset  Size  Field
//   0       2     magic       = 0x4F 0x54 ("OT")
//   2       1     op
//   3       3     payload_len (uint24, little-endian)
//   6       N     payload     (CBOR for control ops; raw bytes for DATA)

const OTA_MAGIC: [u8; 2] = [0x4F, 0x54];
/// CBOR encoding of `{}` (major type 5, 0 entries) — the END payload.
const CBOR_EMPTY_MAP: [u8; 1] = [0xA0];

fn ota_frame(op: u8, payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(6 + payload.len());
    frame.extend_from_slice(&OTA_MAGIC);
    frame.push(op);
    let len = payload.len() as u32;
    frame.extend_from_slice(&len.to_le_bytes()[..3]);
    frame.extend_from_slice(payload);
    frame
}

/// Demuxes framed responses from interleaved boot-log text (ota.md's
/// "Frame reader: robust to log noise") — the 2-byte magic can occur inside
/// ordinary log output (e.g. "OTA active"), so a match is only trusted once
/// its op and payload length both look like a real response frame.
struct FrameReader {
    buf: Vec<u8>,
}

impl FrameReader {
    fn new() -> Self {
        Self { buf: Vec::new() }
    }

    /// Non-blocking: only reads what the OS already has buffered, never
    /// waits. Safe to call after every DATA chunk during streaming without
    /// taxing throughput.
    fn pump_nonblocking(&mut self, port: &mut dyn serialport::SerialPort) -> Result<(), String> {
        let n = match port.bytes_to_read() {
            Ok(n) => n as usize,
            Err(_) => return Ok(()), // treat "can't query" as "nothing available"
        };
        if n == 0 {
            return Ok(());
        }
        let mut tmp = vec![0u8; n.min(4096)];
        match port.read(&mut tmp) {
            Ok(read) => {
                self.buf.extend_from_slice(&tmp[..read]);
                Ok(())
            }
            Err(e) if e.kind() == ErrorKind::TimedOut => Ok(()),
            Err(e) => Err(e.to_string()),
        }
    }

    /// Blocks up to the port's own read timeout (short — see caller's
    /// deadline loop for the real wait budget).
    fn pump_blocking(&mut self, port: &mut dyn serialport::SerialPort) -> Result<(), String> {
        let mut tmp = [0u8; 512];
        match port.read(&mut tmp) {
            Ok(0) => Ok(()),
            Ok(n) => {
                self.buf.extend_from_slice(&tmp[..n]);
                Ok(())
            }
            Err(e) if e.kind() == ErrorKind::TimedOut => Ok(()),
            Err(e) => Err(e.to_string()),
        }
    }

    /// Extracts exactly one complete frame from the buffer, if present.
    /// Silently drops log-noise bytes (and resyncs past false-positive
    /// magics) along the way.
    fn extract(&mut self) -> Option<(u8, Vec<u8>)> {
        loop {
            if self.buf.len() < 2 {
                return None;
            }
            let idx = self.buf.windows(2).position(|w| w == OTA_MAGIC);
            let Some(idx) = idx else {
                // Hold back a trailing lone 0x4F (possible first half of a magic).
                if self.buf.last() == Some(&0x4F) {
                    let keep = self.buf.len() - 1;
                    self.buf.drain(..keep);
                } else {
                    self.buf.clear();
                }
                return None;
            };
            if idx > 0 {
                self.buf.drain(..idx);
                continue;
            }
            if self.buf.len() < 6 {
                return None; // header incomplete — wait for more
            }
            let op = self.buf[2];
            let plen = u32::from_le_bytes([self.buf[3], self.buf[4], self.buf[5], 0]) as usize;
            if !VALID_RESP_OPS.contains(&op) || plen > MAX_RESP_PAYLOAD {
                // False magic inside log text — drop just the first byte, resync.
                self.buf.drain(..1);
                continue;
            }
            if self.buf.len() < 6 + plen {
                return None; // payload incomplete — wait for more
            }
            let payload = self.buf[6..6 + plen].to_vec();
            self.buf.drain(..6 + plen);
            return Some((op, payload));
        }
    }
}

fn wait_frame(
    port: &mut dyn serialport::SerialPort,
    reader: &mut FrameReader,
    want: &[u8],
    timeout: Duration,
) -> Result<(u8, Vec<u8>), String> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some((op, payload)) = reader.extract() {
            if want.contains(&op) {
                return Ok((op, payload));
            }
            continue;
        }
        if Instant::now() >= deadline {
            return Err("No response from Ori — check the cable, and that Ori isn't already mid-update.".to_string());
        }
        reader.pump_blocking(port)?;
    }
}

fn try_read_frame(port: &mut dyn serialport::SerialPort, reader: &mut FrameReader) -> Result<Option<(u8, Vec<u8>)>, String> {
    if let Some(f) = reader.extract() {
        return Ok(Some(f));
    }
    reader.pump_nonblocking(port)?;
    Ok(reader.extract())
}

// ── CBOR payloads (full field names — ota.md's own schema, distinct from
//    the single-char keys the BLE Ori Sync Service uses) ─────────────────

#[derive(Serialize)]
struct OtaBegin<'a> {
    fw_version: &'a str,
    total_size: u64,
    #[serde(with = "serde_bytes")]
    sha256: &'a [u8],
}

fn cbor_begin(fw_version: &str, total_size: u64, sha256: &[u8; 32]) -> Vec<u8> {
    let mut buf = Vec::new();
    let _ = ciborium::into_writer(&OtaBegin { fw_version, total_size, sha256: sha256.as_slice() }, &mut buf);
    buf
}

fn decode_reason(payload: &[u8]) -> String {
    cbor_get_text(payload, "reason").unwrap_or_else(|| "unknown".to_string())
}

fn decode_progress(payload: &[u8]) -> u64 {
    cbor_get_u64(payload, "bytes_received").unwrap_or(0)
}

fn cbor_get_text(payload: &[u8], key: &str) -> Option<String> {
    let v: ciborium::value::Value = ciborium::from_reader(payload).ok()?;
    let map = v.as_map()?;
    let (_, val) = map.iter().find(|(k, _)| k.as_text() == Some(key))?;
    val.as_text().map(|s| s.to_string())
}

fn cbor_get_u64(payload: &[u8], key: &str) -> Option<u64> {
    let v: ciborium::value::Value = ciborium::from_reader(payload).ok()?;
    let map = v.as_map()?;
    let (_, val) = map.iter().find(|(k, _)| k.as_text() == Some(key))?;
    val.as_integer().and_then(|i| i.try_into().ok())
}

/// Reads the version stamped inside the image (the `"OriFwVer=<ver>"`
/// marker) — the same value firmware compares the BEGIN claim against
/// (ota.md § "Version & rollback policy"). Never hardcode/guess this.
fn extract_fw_version(image: &[u8]) -> Option<String> {
    const MARKER: &[u8] = b"OriFwVer=";
    let pos = image.windows(MARKER.len()).position(|w| w == MARKER)?;
    let start = pos + MARKER.len();
    let end = image[start..].iter().position(|&b| b == 0)? + start;
    Some(String::from_utf8_lossy(&image[start..end]).into_owned())
}

/// Maps wire reason codes (ota.md § Errors / § "Error tracking") to a
/// user-facing sentence — same spirit as firmware's own `friendly_reason()`
/// for its on-screen error, just the Orion-side equivalent.
fn friendly_reason(code: &str) -> String {
    match code {
        "too_large" => "This firmware image is too large for Ori's update slot.",
        "missing_fields" | "cbor_decode" | "not_map" => "Ori rejected the update request (internal protocol error).",
        "no_memory" => "Ori is low on memory — unplug and replug it, then try again.",
        "busy" => "An update is already in progress on Ori.",
        "version_mismatch" => "The firmware file's version doesn't match its own version marker — try a different file.",
        "bad_image" => "This doesn't look like a valid Ori firmware file.",
        "hash_mismatch" => "The transfer was corrupted — try again.",
        "truncated" => "The transfer was interrupted — try again.",
        "size_overflow" => "More data was sent than declared — try again.",
        "flash_error" => "Ori failed to write the new firmware — try again.",
        "usb_timeout" => "Connection to Ori was lost during the update — check the cable and try again.",
        other => return format!("Update failed ({other})."),
    }
    .to_string()
}
