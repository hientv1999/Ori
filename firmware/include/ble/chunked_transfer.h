#pragma once

#include <stdint.h>
#include <stddef.h>
#include <functional>

// Ori chunked transfer reassembly engine (ble-protocol.md §5).
//
// Used by: Profile Photo, Meeting List, PTO Entry, Media Album Art.
//
// Frame format (6-byte header):
//   Offset  Size  Field
//   0       2     seq_num     (uint16 LE) — 0-based fragment index
//   2       2     total_frags (uint16 LE)
//   4       2     payload_len (uint16 LE)
//   6       N     payload
//
// Reassembly rules:
//   - Fragments must arrive in order (seq_num == expected).
//   - Gap → callback called with nullptr (NACK_CHUNK_MISSING).
//   - 10 s timeout with no new fragment → callback called with nullptr (NACK_CHUNK_TIMEOUT).
//   - Last fragment (seq_num == total_frags - 1) → reassemble and call callback.
//   - Buffer is PSRAM-allocated to fit the total received payload.

namespace chunked_transfer {

// Completion callback: called on success with the assembled buffer (PSRAM),
// or with (nullptr, 0) on NACK. Caller owns the PSRAM buffer on success and
// must call free() when done (or pass ownership to an lv_image, etc.).
// The `nack_reason` string is only populated on failure.
using CompleteCb = std::function<void(uint8_t* buf, size_t len, const char* nack_reason)>;

// Per-fragment callback: called after every successfully received fragment,
// including the last. seq is 0-based; total is total_frags; payload_len is the
// number of payload bytes carried by this fragment (excludes the 6-byte frame
// header). Use this to post incremental progress events without waiting for
// completion — payload_len is what counts towards SyncControl.total (ble-protocol.md §6.0).
using FragmentCb = std::function<void(uint16_t seq, uint16_t total, uint16_t payload_len)>;

// One reassembly context (one per characteristic that uses chunking).
struct Context {
    uint8_t*    buf       = nullptr; // PSRAM accumulation buffer
    size_t      buf_len   = 0;       // total allocated bytes
    size_t      received  = 0;       // bytes accumulated so far
    uint16_t    expected_seq = 0;    // next expected seq_num
    uint16_t    total_frags  = 0;
    uint32_t    last_frag_ms = 0;    // millis() of last received fragment
    bool        active    = false;
    CompleteCb  on_complete;
    FragmentCb  on_fragment;         // optional; fired after every fragment
};

// Feed one raw BLE write payload (including the 6-byte header) into ctx.
// Returns true if processing succeeded (even partially), false on frame error.
// The on_complete callback (set via ctx.on_complete before first feed) is
// invoked when the transfer completes or times out.
bool feed(Context* ctx, const uint8_t* data, uint16_t len);

// Poll all active contexts for timeouts. Call once per second from
// the ble_manager event loop or the LVGL tick timer.
// Returns the number of contexts that timed out.
int poll_timeouts(Context** ctxs, size_t count);

// Reset a context to idle (frees any partial PSRAM buffer).
void reset(Context* ctx);

// Timeout threshold per spec: 10 seconds.
constexpr uint32_t TIMEOUT_MS = 10000;

} // namespace chunked_transfer
