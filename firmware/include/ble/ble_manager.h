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

// ── Advertising state machine ──────────────────────────────────────────────

// Restart advertising according to the current bond state.
// Safe to call multiple times; stops any existing adv before restarting.
void restart_advertising();

// ── Bond address helpers ───────────────────────────────────────────────────

// Load/save the 6-byte peer address for the Orion or iPhone slot.
// All-zero address means the slot is empty.
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
bool is_iphone_connected();

// Returns the NimBLE conn_handle for the Orion connection, or
// BLE_HS_CONN_HANDLE_NONE if not connected.
uint16_t orion_conn_handle();

// ── Callbacks from GATT server (called from NimBLE task) ──────────────────

// Called when Orion bond is formed (Step 2 complete).
void on_orion_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]);

// Called when iPhone bond is formed (Step 4 / re-pair).
void on_iphone_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]);

// Called when any peer disconnects.
void on_peer_disconnect(uint16_t conn_handle, int reason);

// Called when a 6-digit passkey needs to be displayed (LE SC Passkey Entry).
void on_passkey_display(uint32_t passkey);

// Called when Device Status should be notified to Orion (from GATT server).
void notify_device_status(uint8_t status_byte);

} // namespace ble_manager
