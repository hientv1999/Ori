#pragma once

#include <stdint.h>

// Ori — State Machine (M4)
//
// Owns the left-panel priority logic, periodic ticks, 5-minute pre-meeting
// alert, work-hours boundary, PTO window detection, and meeting expiry.
//
// Priority order (highest → lowest):
//   1. SETUP           — first-boot / factory-reset setup flow
//   2. OTA_UPDATING    — firmware update in progress (future M5 trigger)
//   3. PTO_ACTIVE      — current time within cached PTO window
//   4. COUNTDOWN       — 5-min pre-meeting alert modal
//   5. RECONNECT_SYNCING — Orion reconnected, hash-manifest in progress (M5)
//   6. MEETING_LIST    — work hours (08:00–17:00) with meetings
//   7. NO_MEETINGS     — work hours, no meetings
//   8. CLOCK           — after hours
//
// The state machine also owns the long-press handlers that fire on the profile
// photo (factory reset) and phone-disconnect icon (re-pair phone).

enum class AppState : uint8_t {
    SETUP,
    OTA_UPDATING,
    PTO_ACTIVE,
    COUNTDOWN,
    RECONNECT_SYNCING,
    MEETING_LIST,
    NO_MEETINGS,
    CLOCK,
};

namespace state_machine {

// Initialise the state machine and create the periodic evaluation timer.
// Call once from setup() after nvs::init() and screen_manager::init().
void init();

// Re-evaluate priority and push a new LVGL screen if the state changed.
// Called by the internal lv_timer; may also be called directly when data
// changes (e.g. after NVS wipe, after mode toggle).
AppState evaluate();

// ── Callbacks wired from screen code ──────────────────────────────────────

// User confirmed "LET'S GET TO WORK" on the Setup Complete screen,
// or the 5 s auto-advance timer fired.
void on_setup_complete();

// User confirmed the Factory Reset action (modal Reset button).
void on_factory_reset();

// User confirmed the Unpair Phone action.
void on_unpair_phone();

// User tapped the mode-toggle button in the status bar.
void on_mode_toggle();

// Orion began a firmware update over USB CDC (M5 will call this).
void on_ota_begin();

// BLE reconnect to Orion began — hash-manifest flow starting (M5).
void on_reconnect_begin();

// BLE reconnect to Orion finished — Device Status → RUNTIME_READY (M5).
void on_reconnect_end();

// ── Runtime state setters (called by BLE layer in M5) ─────────────────────

// Update whether the BLE PC link is currently up.
void set_pc_connected(bool connected);

// Update whether a phone BLE bond / link exists.
void set_phone_connected(bool connected);

// Query the current active AppState (for polling).
AppState current_state();

// Query the current mode (Calendar vs Controls).
// Returns 0 for Calendar, 1 for Controls (matches NVS encoding).
uint8_t current_mode();

} // namespace state_machine
