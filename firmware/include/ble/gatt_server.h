#pragma once

#include <stdint.h>
#include <stdbool.h>

// Ori GATT Server — 18 characteristics per ble-protocol.md v1.0 (chars 16-18
// added for the ANCS relay, §13), plus the BLE SIG standard Device
// Information Service (Firmware Revision String).
//
// Service UUID: 6F726900-0000-4F72-9F00-000000000000
// Each characteristic UUID has bytes 4-5 replaced with the offset below.
//
// This file owns:
//   - Service + characteristic registration with NimBLE
//   - Read/Write callbacks for all 15 chars
//   - Chunked reassembly (delegates to chunked_transfer.h for photo/meetings/Time Off/art)
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

// Poll all chunk-reassembly contexts (Profile Photo, Meeting List, Time Off,
// Media Album Art) for the 10 s no-progress timeout (ble-protocol.md §5).
// Frees the PSRAM staging buffer and NACKs via on_complete for any context
// that stalled. Call once per second from ble_manager::poll().
void poll_chunk_timeouts();

// Apply all staged sync data to NVS/UI in one burst (§6.0), then transition
// Device Status and signal SyncEnd. Must run on the main task — call only
// from ble_manager::poll() in response to a deferred SyncCommit event.
void run_staged_commit();

// Notify Orion of the current iPhone bond/connection state and update the
// readable characteristic value. Called from ble_manager::poll() whenever
// iPhone state changes (bonded, connected, disconnected, or bond wiped).
// bonded: the iPhone NVS slot is occupied.
// connected: the BLE link to the iPhone is currently up.
// name: GAP Device Name read from the iPhone (e.g. "Xander's iPhone"), or ""
//       if the phone is not connected or the read failed. Encrypted (READ_ENC).
void notify_phone_bond_status(bool bonded, bool connected, const char* name);

// Push updated iPhone ANCS stats (missed calls, unread messages, and all
// other active notifications — the three counts are mutually exclusive) and
// signal strength (0-4 bars, from live connection RSSI) to Orion via the
// same Phone Bond Status characteristic — called from
// ancs_client whenever the notification queue changes or the periodic RSSI
// poll detects a change. Re-encodes using the bonded/connected/name last set
// by notify_phone_bond_status(); a no-op while disconnected (nothing to
// relay) or when none of the four values actually changed.
void notify_phone_stats(uint8_t missed, uint8_t unread, uint8_t total, uint8_t signal_bars);

// ── ANCS relay to Orion (chars 0010-0012, ble-protocol.md §13) ────────────
// Owned by ancs_client, which calls these whenever a relayed notification's
// state changes; ancs_client also owns applying the ancs_filter gate before
// calling notify_ancs_add() — these functions send exactly what they're given.

// Relay AncsNotification{op:"add", ...} (char 0010). Every text field is
// UTF-8-boundary-truncated to its ble-protocol.md §10 wire cap internally —
// callers may pass the full stored string. Never call for a call notification
// (AncsCategory INCOMING_CALL/ACTIVE_CALL) — those relay exclusively via
// notify_ancs_call_state() below.
void notify_ancs_add(uint32_t uid, const char* icon_token, uint8_t category,
                      const char* app, const char* title, const char* body,
                      uint32_t recv_epoch, const char* pos_label,
                      const char* neg_label, bool has_neg_action, bool silent);

// Relay AncsNotification{op:"remove", u:uid} (char 0010).
void notify_ancs_remove(uint32_t uid);

// Relay AncsNotification{op:"clear"} (char 0010) — wipe Orion's entire local
// mirror. Sent once before re-sending "add" for everything that passes a
// newly-changed ancs_filter (ble-protocol.md §13's "Filter changes re-evaluate
// live state").
void notify_ancs_clear();

// Relay AncsCallState{st, u:uid, e:elapsed_s, a,t,p,n,g} (char 0011). st:
// 0=none/ended, 1=ringing, 2=active. elapsed_s is only meaningful when
// st==2. app/title/pos_label/neg_label/has_neg_action carry the caller's
// identity + action button labels so Orion's ringing/on-call view never has
// to guess them — pass "" / false / nullptr for st==0 (nothing to show).
// Never fabricate pos_label/neg_label text; pass through exactly what ANCS
// gave the caller of this function (same "don't hardcode" rule as chars
// 0010/0012's action labels).
void notify_ancs_call_state(uint8_t st, uint32_t uid, uint32_t elapsed_s,
                             const char* app = nullptr, const char* title = nullptr,
                             const char* pos_label = nullptr, const char* neg_label = nullptr,
                             bool has_neg_action = false, const char* icon_token = nullptr);

// Send a SyncControl{op:"NACK", reason} notification (char 0007) — exposed
// for ble_manager::poll() to reuse when an ANCS Notification Action (char
// 0012) targets a uid that's no longer live in ancs_client's queue
// (ble-protocol.md §13: "NACK via the existing SyncControl NACK_CBOR_DECODE
// path, reused"). Every other NACK on this char is still sent inline by the
// write handler that detects it, same as before.
void nack_sync_control(const char* reason);

} // namespace gatt_server
