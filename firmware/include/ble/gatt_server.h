#pragma once

#include <stdint.h>
#include <stdbool.h>

// Ori GATT Server — all 15 characteristics per ble-protocol.md v1.0.
//
// Service UUID: 6F726900-0000-4F72-9F00-000000000000
// Each characteristic UUID has bytes 4-5 replaced with the offset below.
//
// This file owns:
//   - Service + characteristic registration with NimBLE
//   - Read/Write callbacks for all 15 chars
//   - Chunked reassembly (delegates to chunked_transfer.h for photo/meetings/PTO/art)
//   - SHA-256 hash computation and NVS save after SyncControl{op:"END"}
//   - State machine transitions triggered by GATT events
//   - BLE data characteristic NACKing while OTA is active

namespace gatt_server {

// Register the Ori Sync Service with NimBLE. Call once from ble_manager::init()
// BEFORE NimBLE is started.
void init();

// Returns true if a firmware OTA is currently in progress (BLE writes NACKed).
bool is_ota_active();

// Called by ota_receiver when OTA begins/ends.
void set_ota_active(bool active);

// Update the Device Status characteristic value and notify Orion if connected.
// Status byte values defined in ble-protocol.md §4:
//   0x00 SETUP_WAITING_PAIRING
//   0x01 SETUP_BONDED_AWAITING_SYNC
//   0x02 SETUP_SYNCING
//   0x03 SETUP_SYNC_COMPLETE
//   0x10 RUNTIME_READY
//   0x11 RUNTIME_RECONNECTING
//   0x12 RUNTIME_SYNCING
//   0xF0 ERROR_GENERIC
void set_device_status(uint8_t status);
uint8_t get_device_status();

// Send a KeyboardCommand notification to the connected Orion peer.
// op: "vol_set"|"play_pause"|"prev"|"next"|"shortcut"|"seek"
// arg: volume level (0..100), shortcut slot (1..3), seek position (seconds)
void notify_keyboard_command(const char* op, uint32_t arg);

// Discard any in-progress sync staging (PSRAM buffers + byte counters) and
// clear the sync-in-progress flag. Call on Orion disconnect so a sync
// session aborted mid-transfer (no SyncControl{END}) doesn't leak PSRAM or
// leave stale staged data for the next BEGIN. ble-protocol.md §6.0/§7.
void abort_sync_stage();

// Apply all staged sync data to NVS/UI in one burst (§6.0), then transition
// Device Status and signal SyncEnd. Must run on the main task — call only
// from ble_manager::poll() in response to a deferred SyncCommit event.
void run_staged_commit();

} // namespace gatt_server
