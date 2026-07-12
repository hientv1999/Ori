// Chunking protocol — ble-protocol.md §5. Frame: seq_num(u16 LE) |
// total_frags(u16 LE) | payload_len(u16 LE) | payload. Mirrors
// tools/mock_orion_ble.py's make_frames()/write_chunked() byte-for-byte.

/// Per-fragment payload at MTU 247: 247 - 3 (ATT header) - 6 (frame header).
/// Also the ceiling `frag_size_for_mtu` clamps to — ble-protocol.md §5 was
/// designed and firmware's reassembly buffer sized around this figure, so a
/// larger negotiated MTU doesn't grow fragments past it.
pub const FRAG_SIZE: usize = 238;

/// Windowed flow control (§5): stream Write-No-Response, with a Write-with-
/// response checkpoint every WINDOW fragments (and on the last one) so the
/// sender never runs more than one window ahead of Ori's RX buffer.
pub const CHUNK_WINDOW: usize = 32;

/// Per-fragment payload size for a given negotiated ATT MTU (`mtu - 3 ATT
/// header - 6 frame header`, capped at `FRAG_SIZE`). Windows negotiates ATT
/// MTU automatically on connect and doesn't expose an app-level "request
/// 247" API (unlike Android/BlueZ) — btleplug's `Peripheral::mtu()` only
/// reports whatever the OS actually settled on. This makes the wire format
/// adapt to that real value instead of blindly assuming the 247-byte best
/// case: a peripheral that only grants the 23-byte default still gets
/// correctly-sized (if smaller and more numerous) fragments rather than
/// writes larger than the negotiated MTU allows.
pub fn frag_size_for_mtu(mtu: u16) -> usize {
    (mtu as usize).saturating_sub(3 + 6).clamp(1, FRAG_SIZE)
}

/// Splits `payload` into wire frames of at most `frag_size` bytes each —
/// see `frag_size_for_mtu`. An empty payload still produces one zero-length
/// frame — that's how Ori is told "no data" for this item.
///
/// `seq_num`/`total_frags` are wire `u16` fields (§5's frame format) — the
/// `chunks.len() as u16` / `i as u16` casts below silently wrap instead of
/// erroring if fragment count ever exceeds 65535. That can't happen today:
/// the real-world ATT MTU floor (23 — BLE's own spec minimum, and
/// btleplug's `DEFAULT_MTU_SIZE`) keeps `frag_size_for_mtu` at ≥ 14, and even
/// the largest capped payload (`TIME_OFF_PHOTO_MAX_BYTES` = 512 KB, in
/// `central.rs`) divides to ~37,450 fragments — comfortably under
/// `u16::MAX`. The `debug_assert!`s below are a tripwire against that
/// relationship silently breaking in the future (a bumped `*_MAX_BYTES` cap,
/// or a lower real-world MTU floor), so it fails loudly in debug builds
/// instead of corrupting the wire frames.
pub fn make_frames(payload: &[u8], frag_size: usize) -> Vec<Vec<u8>> {
    if payload.is_empty() {
        return vec![frame(0, 1, &[])];
    }
    let chunks: Vec<&[u8]> = payload.chunks(frag_size.max(1)).collect();
    debug_assert!(
        chunks.len() <= u16::MAX as usize,
        "chunk total ({}) exceeds u16::MAX — total_frags would silently wrap on the wire",
        chunks.len()
    );
    let total = chunks.len() as u16;
    chunks
        .iter()
        .enumerate()
        .map(|(i, c)| {
            debug_assert!(i <= u16::MAX as usize, "fragment index ({i}) exceeds u16::MAX — seq_num would silently wrap on the wire");
            frame(i as u16, total, c)
        })
        .collect()
}

fn frame(seq: u16, total: u16, payload: &[u8]) -> Vec<u8> {
    debug_assert!(
        payload.len() <= u16::MAX as usize,
        "fragment payload ({} bytes) exceeds u16::MAX — payload_len would silently wrap on the wire",
        payload.len()
    );
    let mut buf = Vec::with_capacity(6 + payload.len());
    buf.extend_from_slice(&seq.to_le_bytes());
    buf.extend_from_slice(&total.to_le_bytes());
    buf.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    buf.extend_from_slice(payload);
    buf
}

/// Returns, for each frame index, whether it should be sent as a
/// Write-with-response checkpoint (every `CHUNK_WINDOW`th fragment, and
/// always the last one).
pub fn is_checkpoint(index: usize, total_frames: usize) -> bool {
    (index + 1) % CHUNK_WINDOW == 0 || index + 1 == total_frames
}

/// Reassembles the REVERSE direction of the frame format above — Ori is the
/// sender here, Orion the receiver (ble-protocol.md §5's "AncsNotification
/// chunking", char 0010 only today). Deliberately simpler than the
/// write-direction protocol: no NACK/retry channel exists on a notify-only
/// characteristic, so a gap or desync just discards the partial buffer and
/// waits for the next `seq_num == 0` to resync — the corresponding queue
/// mutation on Ori re-sends the notification anyway (`ancs_client.cpp`'s
/// add/remove/clear triggers), so a dropped sequence self-heals without an
/// explicit retry request. One instance per notify stream that uses this
/// (one per Ori connection, held for that connection's lifetime).
#[derive(Default)]
pub struct Reassembler {
    expected_seq: u16,
    total_frags: u16,
    buf: Vec<u8>,
}

impl Reassembler {
    pub fn new() -> Self {
        Self::default()
    }

    /// Feeds one raw notify payload (including the 6-byte frame header).
    /// Returns `Some(complete_bytes)` once the last fragment of a sequence
    /// arrives (immediately, for a single-frame `total_frags:1` sequence);
    /// `None` while still accumulating, or after silently dropping a
    /// malformed/out-of-sync frame (module doc comment above).
    pub fn feed(&mut self, data: &[u8]) -> Option<Vec<u8>> {
        if data.len() < 6 {
            return None; // too short to be a valid frame — drop
        }
        let seq = u16::from_le_bytes([data[0], data[1]]);
        let total = u16::from_le_bytes([data[2], data[3]]);
        let payload_len = u16::from_le_bytes([data[4], data[5]]) as usize;
        let rest = &data[6..];
        if rest.len() < payload_len {
            return None; // truncated frame — drop
        }
        let payload = &rest[..payload_len];

        if seq == 0 {
            // Start of a fresh sequence — unconditionally, even if a prior
            // sequence was left incomplete (e.g. a dropped fragment further
            // back): the newest seq==0 always wins, so a stalled reassembly
            // can't wedge this reassembler against a genuinely new one.
            self.total_frags = total;
            self.buf.clear();
            self.buf.reserve(payload_len.saturating_mul(total.max(1) as usize));
        } else if seq != self.expected_seq || total != self.total_frags {
            // Gap or desync mid-sequence — discard and wait for the next
            // seq==0 (see module doc comment: this self-heals).
            self.expected_seq = 0;
            self.total_frags = 0;
            self.buf.clear();
            return None;
        }

        self.buf.extend_from_slice(payload);
        self.expected_seq = seq + 1;

        if self.expected_seq == self.total_frags {
            self.expected_seq = 0;
            self.total_frags = 0;
            Some(std::mem::take(&mut self.buf))
        } else {
            None
        }
    }
}
