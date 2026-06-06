#pragma once

#include <stdint.h>
#include <stdbool.h>

// Ori USB CDC OTA receiver (ota.md).
//
// Reads the framed OTA protocol from the USB CDC serial port (Serial, not Serial0).
// Magic bytes 0x4F54 ("OT") distinguish OTA frames from boot log bytes.
//
// Frame format:
//   Offset  Size  Field
//   0       2     magic       = 0x4F54
//   2       1     op
//   3       3     payload_len (uint24 LE)
//   6       N     payload     (CBOR for control ops, raw bytes for DATA)
//
// Op codes:
//   0x01 BEGIN       PC → Ori   CBOR: { fw_version, total_size, sha256 }
//   0x02 READY       Ori → PC   CBOR: {}
//   0x03 REJECT      Ori → PC   CBOR: { reason }
//   0x04 DATA        PC → Ori   raw bytes
//   0x05 PROGRESS    Ori → PC   CBOR: { bytes_received }
//   0x06 END         PC → Ori   CBOR: {}
//   0x07 VALIDATED   Ori → PC   CBOR: {}
//   0x08 FAILED      Ori → PC   CBOR: { reason }
//
// Reject conditions (ota.md):
//   - countdown modal active
//   - total_size > inactive slot capacity
//   - fw_version matches current firmware
//
// While OTA is in progress: all BLE data characteristic writes are NACKed
// (gatt_server::is_ota_active() returns true).

namespace ota_receiver {

// Initialise the OTA receiver state machine. Call once from setup().
void init();

// Poll the USB CDC serial port for incoming OTA frames.
// Call every loop() iteration. Non-blocking.
// When a complete OTA sequence is detected, this calls:
//   - state_machine::on_ota_begin() to show the OTA screen
//   - gatt_server::set_ota_active(true)
//   - Arduino Update library to flash the inactive slot
//   - screen_ota_updating progress ring updates
//   - state_machine transitions on completion/failure
void poll();

// Returns true when an OTA update is currently in progress.
bool is_active();

// Update the OTA progress ring (0..100%). Called internally from poll()
// when a PROGRESS frame is assembled; also callable externally for testing.
void set_progress(uint8_t pct);

// Returns the current firmware version string (semver, e.g. "1.0.0").
// Used by gatt_server to fill the Protocol Version characteristic.
const char* firmware_version();

} // namespace ota_receiver
