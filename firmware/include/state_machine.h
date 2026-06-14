#pragma once

#include <stddef.h>
#include <stdint.h>

// Forward declaration — avoids pulling all of lvgl.h into this widely-included
// header just for the ota_show() screen-handoff parameter.
typedef struct _lv_obj_t lv_obj_t;

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
    OTA_ACK,          // post-update "Firmware updated" ack (persists until Close)
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

// Parse a raw MeetingList CBOR blob (from BLE) into the runtime meeting cache.
// Meetings are RAM-only — NOT persisted to NVS: a power cycle drops the local
// clock (it is never restored from flash), so the meeting time logic (5-minute
// alert, in-progress red, expiry) couldn't run, and showing stale meetings
// would mislead. Orion re-pushes the list on reconnect. Call from the BLE
// MeetingList handler.
void set_meetings_cbor(const uint8_t* buf, size_t len);

// The PTO destination image finished decoding into photo_cache after the
// screen was already built (the image streams in asynchronously). Unlike the
// profile photo — which widget_profile_card::set_photo() live-updates on the
// active card — the PTO screen has no in-place image setter, so this forces a
// rebuild when PTO_ACTIVE is the screen currently shown (cheap no-op otherwise).
// Call from the BLE PtoPhotoReceived handler after photo_cache::store_pto().
void notify_pto_image_changed();

// Re-evaluate priority and push a new LVGL screen if the state changed.
// Called by the internal lv_timer; may also be called directly when data
// changes (e.g. after NVS wipe, after mode toggle).
AppState evaluate();

// ── Callbacks wired from screen code ──────────────────────────────────────

// Called when the Setup-Complete screen appears: holds the state machine on the
// current screen so a tick / BLE-driven evaluate() can't rebuild to the runtime
// screen mid-animation (mark_setup_complete() has already flipped is_first_boot).
// Released by on_setup_complete().
void hold_for_setup_complete();

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

// Load a full-screen OTA takeover screen built by ota_receiver (Firmware
// Install / Update failed). Keeps g_state = OTA_UPDATING so it stays sticky.
void ota_show(lv_obj_t* screen);

// User tapped Close on the post-update acknowledgement screen — clear the NVS
// flag and return to normal runtime.
void on_ota_ack_close();

// BLE reconnect to Orion began — hash-manifest flow starting (M5).
void on_reconnect_begin();

// BLE reconnect to Orion finished — Device Status → RUNTIME_READY (M5).
void on_reconnect_end();

// ── Runtime state setters (called by BLE layer in M5) ─────────────────────

// Update whether the BLE PC link is currently up.
void set_pc_connected(bool connected);

// Cache the most recent Teams presence pushed by Orion via the Presence
// Status characteristic (0x00 Available .. 0x03 Offline — ble-protocol.md
// §3). apply_widget_defaults() reflects this value (instead of a hardcoded
// one) on every screen rebuild while the PC link is up, and falls back to
// Offline while it's down.
void set_presence(uint8_t presence_byte);

// Update whether a phone BLE bond / link exists.
void set_phone_connected(bool connected);

// Wipe a stale iPhone bond (bonded but disconnected) so the re-pair screen
// can actually pair: defers the NVS write + advertising restart to poll(),
// without rebuilding the current screen. Called from the status-bar phone
// icon before loading the re-pair screen.
void request_phone_bond_wipe();

// Query the current active AppState (for polling).
AppState current_state();

// User tapped the status-bar time — enter Clock mode.
// Saves the current mode so on_mode_toggle() can return to it.
void on_clock_enter();

// Query the current mode.
// Returns 0 = Calendar (meeting list), 1 = Media.
uint8_t current_mode();

} // namespace state_machine
