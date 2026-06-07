#pragma once

#include <stddef.h>
#include <stdint.h>

// Ori — State Machine (M4)
//
// Owns the left-panel priority logic, periodic ticks, 5-minute pre-meeting
// alert, PTO window detection, and meeting expiry.
//
// Priority order (highest → lowest):
//   1. SETUP             — first-boot / factory-reset setup flow
//   2. OTA_UPDATING      — firmware update in progress (future M5 trigger)
//   3. PTO_ACTIVE        — current time within cached PTO window
//   4. COUNTDOWN         — 5-min pre-meeting alert modal
//   5. RECONNECT_SYNCING — Orion reconnected, hash-manifest in progress (M5)
//   6+. Mode-driven (g_mode):
//       MEETING_LIST / NO_MEETINGS — Calendar mode (mode=0, default)
//       [Media rendered via MEETING_LIST/NO_MEETINGS path — mode=1]
//   7.  CLOCK — user-entered by tapping the status-bar time; not in the
//       mode-toggle cycle; exits via the mode-toggle button (returns to the
//       mode that was active before the tap).
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

// Drain pending deferred work (NVS writes, screen transitions) that must not
// run inside an LVGL timer callback. Call every loop() iteration BEFORE
// lv_timer_handler().
void poll();

// Parse a raw MeetingList CBOR blob (from BLE or NVS) into the runtime meeting
// cache. Also saves the blob to NVS for persistence across reboots.
// Call from the BLE MeetingList handler and from boot (with the NVS blob).
void set_meetings_cbor(const uint8_t* buf, size_t len, bool save_to_nvs);

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

// User tapped the status-bar time — enter Clock mode.
// Saves the current mode so on_mode_toggle() can return to it.
void on_clock_enter();

// Query the current mode.
// Returns 0 = Calendar (meeting list), 1 = Media.
uint8_t current_mode();

} // namespace state_machine
