#pragma once

#include <stdint.h>
#include <stdbool.h>

// Ori BLE Manager — top-level init, advertising, dual-connection orchestration.
//
// Owns:
//   - NimBLE stack init
//   - Advertising state machine (ble-protocol.md §2)
//   - Dual-connection management (one PC/Orion + one iPhone/ANCS)
//   - Bond slot enforcement (state-gated pairing)
//   - Passkey display events → screen_setup passkey modal
//   - BLE event queue (defers factory_reset::execute() out of callbacks)
//
// Does NOT own:
//   - GATT characteristic handlers → gatt_server.h
//   - ANCS subscription → ancs_client.h
//   - NVS persistence → nvs_sync.h
//
// Threading model: NimBLE runs on its own FreeRTOS task. All events that
// need to touch LVGL objects are posted to g_ble_event_queue and drained
// in ble_manager::poll() which is called from the Arduino loop() via
// main.cpp after the BLE init call.

namespace ble_manager {

// Advertising mode flag bytes (ble-protocol.md §2).
constexpr uint8_t ADV_FLAG_SETUP   = 0x01;
constexpr uint8_t ADV_FLAG_RUNTIME = 0x02;

// Bond-type identifiers stored in NVS namespace "bonds".
// NVS keys are "orion_addr" and "iphone_addr" (6 bytes each, all-zero = empty).
constexpr const char* BOND_NS          = "bonds";
constexpr const char* BOND_KEY_ORION   = "orion_addr";
constexpr const char* BOND_KEY_IPHONE  = "iphone_addr";

// Initialise NimBLE stack, register GATT server, register ANCS client,
// set up advertising. Must be called once from setup() AFTER nvs::init()
// and screen_manager::init() (because passkey events call into screen_setup).
void init();

// Poll the BLE event queue — call every loop() iteration BEFORE lv_timer_handler().
// Drains all pending events that need to run on the Arduino main task
// (LVGL calls, factory reset, state machine transitions, etc.).
void poll();

// Fully tear down the NimBLE stack (host + controller) ahead of the firmware
// flash commit, so nothing BLE-side can execute non-IRAM code or trigger a
// bond/NVS flash write while Update.write() has the MSPI cache disabled.
// One-way: the caller reboots immediately after the commit, which re-inits BLE
// from scratch. Preserves bonds (they live in NVS and are reloaded on boot).
// The image is fully staged in PSRAM by this point, so tearing down the link
// that delivered it costs nothing — but it does mean VALIDATED must already
// have been notified before this is called (ota.md).
void quiesce_for_commit();

// Firmware-transfer quiet mode (ota.md "Behaviour") — the reversible little
// sibling of quiesce_for_commit(). quiet=true (accepted BEGIN): stops
// advertising for the whole download (no reconnect ceremony — the heaviest
// BLE burst in the system — can start mid-stream; restart_advertising()
// no-ops for the duration), suspends ANCS processing at the source
// (ancs_client::suspend_for_ota()), and drops the iPhone link so the whole
// radio schedule belongs to the connection carrying the image. Orion's own
// link is untouched — it IS the transport now. quiet=false (failure-resume):
// lifts the suppression and re-arms advertising, which is what brings the
// phone back; the force-drop for replaying missed ANCS events is a no-op in
// the common case since the link is already down. No-op when never suppressed
// (pre-accept rejects). A successful commit never calls this —
// quiesce_for_commit() + reboot supersedes it.
void set_ota_transfer_quiet(bool quiet);

// Request a tighter (fast=true) or the normal (fast=false) connection interval
// on the Orion link, around a firmware transfer. No-op when Orion isn't
// connected, and best-effort in general: the central may decline, which only
// makes the transfer slower. See the unit note in the implementation.
void set_ota_link_fast(bool fast);

// ── Advertising state machine ──────────────────────────────────────────────

// Restart advertising according to the current bond state.
// Safe to call multiple times; stops any existing adv before restarting.
void restart_advertising();

// Mark whether the iPhone-pairing UI (Setup Step 4 or the runtime re-pair
// screen) is currently on-screen. The ANCS service UUID is only advertised
// while this is true and the iPhone slot is empty — so Ori does not solicit
// iPhone connections in normal runtime, only when the user is actively pairing.
// Restarts advertising on a state change. Call true on entering the pairing
// screen, false on leaving it.
void set_iphone_pairing_window(bool active);

// ── Bond address helpers ───────────────────────────────────────────────────

// Read both bond-slot addresses from NVS into the RAM cache. MUST be called
// once on the main task at boot, before state_machine::init() and before the
// BLE stack starts (ble_manager::init()). After this, load_bond_addr() reads
// the cache (no NVS), so the NimBLE host-task callbacks never open Preferences.
void prime_bond_cache();

// Load/save the 6-byte peer address for the Orion or iPhone slot.
// All-zero address means the slot is empty. load_bond_addr() reads the RAM
// cache (safe from any task); save_bond_addr() updates the cache + NVS and
// must be called on the main task only.
void load_bond_addr(const char* key, uint8_t out_addr[6]);
void save_bond_addr(const char* key, const uint8_t addr[6]);
bool is_bond_slot_empty(const uint8_t addr[6]);

// Clear both NVS bond addresses AND erase the NimBLE bond records.
// Called from factory_reset::execute().
void wipe_all_bonds();

// Clear the iPhone NVS address and NimBLE bond (for re-pair-iPhone flow).
void wipe_iphone_bond();

// ── Runtime state queries ──────────────────────────────────────────────────

bool is_orion_connected();

// Returns the NimBLE conn_handle for the Orion connection, or
// BLE_HS_CONN_HANDLE_NONE if not connected.
uint16_t orion_conn_handle();

// ── Callbacks from GATT server (called from NimBLE task) ──────────────────

// Called when the Orion passkey bond is formed (Step 2). The bond is held
// PROVISIONALLY (RAM only) until the peer proves it's Orion via a valid
// SyncControl{BEGIN}; see confirm_orion_peer().
void on_orion_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]);

// Called from the GATT server when a bonded peer writes a valid
// SyncControl{BEGIN}. If that peer is the provisional Orion, this commits the
// bond to NVS (otherwise a no-op). A provisional peer that never sends BEGIN is
// dropped after a timeout and never saved as Orion.
void confirm_orion_peer();

// Called when iPhone bond is formed (Step 4 / re-pair).
void on_iphone_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]);

// Fires the first-boot ANCS backlog-flush reconnect (see on_iphone_bonded's
// poll() handler) if one is pending. Deliberately NOT called on a fixed timer
// right after bonding — call this once the Setup Complete screen hands off to
// runtime (state_machine::poll()), so the heavier ANCS backlog processing
// doesn't compete with that screen's checkmark/countdown animations. No-op if
// nothing is pending or the iPhone already disconnected on its own since.
void run_pending_ancs_backlog_reconnect();

} // namespace ble_manager
