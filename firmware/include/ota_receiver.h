#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Ori BLE firmware-update receiver (ota.md).
//
// Transport is BLE — two characteristics on the Ori Sync Service, written only
// by the bonded, encrypted Orion peer (ble-protocol.md §14). There is no USB
// firmware-update path: the enclosure exposes no reachable data port, so the
// device updates fully wirelessly in the field.
//
//   0014  Firmware Update Control  Read + Write (response) + Notify
//   0015  Firmware Update Data     Write no-response (+ Write for the
//                                  sender's windowed checkpoint)
//
// Control payloads are CBOR with single-character keys, matching the rest of
// the service (ble-protocol.md §4):
//
//   Orion → Ori   { "o": "BEGIN", "v": <version>, "n": <total_size>, "h": <sha256> }
//                 { "o": "END" }   { "o": "ABORT" }
//   Ori → Orion   { "o": "READY" }
//                 { "o": "REJECT",    "r": <reason> }
//                 { "o": "PROGRESS",  "b": <bytes_received> }
//                 { "o": "RESUME",    "b": <expected_offset> }
//                 { "o": "VALIDATED", "v": <version read from the binary> }
//                 { "o": "FAILED",    "r": <reason> }
//                 { "o": "DISMISSED" }   — the Update-failed screen was closed
//                                          on the device; Orion closes its own
//
// Data frames carry an absolute byte OFFSET, not a fragment index:
//
//   Offset  Size  Field
//   0       4     offset (uint32 LE) — position of this payload in the image
//   4       N     image bytes
//
// The offset makes a lost fragment cost one rewind instead of a whole restart,
// which matters far more over BLE than it did over a cable, and it has no
// fragment-count ceiling (a uint16 index would overflow on a small MTU).
// Ori accepts a frame only at `offset == bytes_received`; anything else is
// answered with RESUME carrying the offset it actually wants.
//
// BEGIN reject reasons: busy, missing_fields, not_map, cbor_decode, too_large,
// no_memory. Post-BEGIN failure reasons: truncated, size_overflow,
// hash_mismatch, bad_image, version_mismatch (the binary's embedded version
// disagrees with the BEGIN claim — same-version reinstalls and downgrades are
// allowed), ble_timeout, link_lost, aborted, flash_error. Full list and
// rationale: ota.md.
//
// Threading: the two on_*_write() entry points run on the NimBLE host task;
// everything that touches LVGL, NVS, or PSRAM allocation is deferred to
// poll() on the Arduino main task. While a transfer is live, every OTHER BLE
// data characteristic is NACKed (gatt_server::is_ota_active()).

namespace ota_receiver {

// Initialise the receiver state machine. Call once from setup().
void init();

// Main-task tick: drives the progress ring, the no-progress watchdog, the
// deferred failure path, and the flash commit. Call every loop() iteration.
void poll();

// Firmware Update Control (char 0014) write — NimBLE host task.
// Parses the op and defers the work to the main task; only the cheap
// synchronous rejects (busy / malformed CBOR) answer inline.
void on_control_write(const uint8_t* cbor, uint16_t len);

// Firmware Update Data (char 0015) write — NimBLE host task, hot path.
// Validates the offset and copies straight into the PSRAM staging buffer.
void on_data_write(const uint8_t* frame, uint16_t len);

// Deferred control ops, drained from ble_manager::poll() on the main task.
void run_begin();
void run_end();
void run_abort();

// Orion's link dropped. Aborts a download in progress (the image is
// incomplete and there is no resume across connections); a transfer already
// past verification is left alone to finish installing.
void on_orion_disconnected();

// True from an accepted BEGIN until the update commits or its error screen is
// dismissed. Gates BLE data writes and makes OTA the top-priority app state.
bool is_active();

// "Close" on the Update failed screen: drop the failed transfer, resume runtime.
void dismiss_error();

} // namespace ota_receiver
