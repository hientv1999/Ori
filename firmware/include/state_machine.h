#pragma once

#include <stddef.h>
#include <stdint.h>

// Forward declaration — avoids pulling all of lvgl.h into this widely-included
// header just for the ota_show() screen-handoff parameter.
typedef struct _lv_obj_t lv_obj_t;

// Forward declaration — avoids pulling app_state.h into this widely-included
// header just for the show_countdown_if_imminent() parameter type.
namespace app_state { struct Meeting; }

// Ori — State Machine
//
// Owns the left-panel priority logic, periodic ticks, 5-minute pre-meeting
// alert, Time Off window detection, and meeting expiry.
//
// Priority order (highest → lowest):
//   1. OTA_UPDATING      — firmware update in progress. Outranks even SETUP:
//                          USB CDC OTA works without a BLE bond (ota.md —
//                          physical cable access is sufficient authority), so
//                          a first-boot unit mid-transfer must not have its
//                          screen yanked to the Setup wizard.
//   2. SETUP             — first-boot / factory-reset setup flow
//   3. TIME_OFF_ACTIVE   — current time within cached Time Off window
//   4. COUNTDOWN         — 5-min pre-meeting alert modal
//   5. RECONNECT_SYNCING — Orion reconnected, hash-manifest sync in progress
//   6+. Mode-driven (g_mode):
//       MEETING_LIST / NO_MEETINGS — Calendar mode (mode=0, default)
//       [Media rendered via MEETING_LIST/NO_MEETINGS path — mode=1]
//   7.  CLOCK — user-entered by tapping the status-bar time; not in the
//       mode-toggle cycle; exits via the mode-toggle button (returns to the
//       mode that was active before the tap).
//   8.  CALENDAR_VIEW — user-entered by long-pressing the status-bar time;
//       same exit mechanics as CLOCK (mode-toggle returns to the prior mode).
//
// The state machine also owns the long-press handlers that fire on the profile
// photo (factory reset), the phone-disconnect icon (re-pair phone), and the
// status-bar time (Calendar month view).

enum class AppState : uint8_t {
    SETUP,
    OTA_UPDATING,
    OTA_ACK,          // post-update "Firmware updated" ack (persists until Close)
    TIME_OFF_ACTIVE,
    COUNTDOWN,
    RECONNECT_SYNCING,
    MEETING_LIST,
    NO_MEETINGS,
    CLOCK,
    CALENDAR_VIEW,
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

// The Time Off destination image finished decoding into photo_cache after the
// screen was already built (the image streams in asynchronously). Unlike the
// profile photo — which widget_profile_card::set_photo() live-updates on the
// active card — the Time Off screen has no in-place image setter, so this forces a
// rebuild when TIME_OFF_ACTIVE is the screen currently shown (cheap no-op otherwise).
// Call from the BLE TimeOffPhotoReceived handler after photo_cache::store_time_off().
void notify_time_off_image_changed();

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

// Orion began a firmware update over USB CDC. Called from ota_receiver.
void on_ota_begin();

// Load a full-screen OTA takeover screen built by ota_receiver (Firmware
// Install / Update failed). Keeps g_state = OTA_UPDATING so it stays sticky.
void ota_show(lv_obj_t* screen);

// User tapped Close on the post-update acknowledgement screen — clear the NVS
// flag and return to normal runtime.
void on_ota_ack_close();

// BLE reconnect to Orion began — hash-manifest flow starting.
void on_reconnect_begin();

// BLE reconnect to Orion finished — Device Status → RUNTIME_READY.
void on_reconnect_end();

// Called by gatt_server::run_staged_commit() whenever this sync actually
// wrote to NVS (Profile, Photo, or Time Off) and therefore blanked the
// framebuffer (lcd_panel::blackout()) ahead of the flash write. That
// blackout is gated purely on WHICH items were staged, entirely independent
// of whether the sync was big enough to have shown the reconnect-syncing
// overlay (RECONNECT_OVERLAY_MIN_BYTES) — a small mid-session push (e.g.
// removing the profile photo, or any profile-only edit) still commits to
// NVS and still blanks the screen, but never sets g_reconnect_overlay_mode.
// on_reconnect_end()'s repaint step used to be gated on that overlay flag
// alone, so a sync like this left the framebuffer blanked with nothing to
// ever redraw it — the status bar, mode-toggle, and meeting list all read
// as empty even though their underlying widgets were untouched. This flag
// lets on_reconnect_end() repaint whenever the blackout genuinely
// happened, regardless of overlay size.
void mark_display_needs_repaint();

// Called by screen_meeting_list::create() / screen_no_meetings::create() to
// register the live calendar-runtime screen's containers, so a reconnect sync
// can refresh ONLY the left panel in place (refresh_runtime_left) instead of
// rebuilding the whole screen — leaving the status bar + profile card (which
// update their own contents live) untouched. Auto-cleared when the screen is
// deleted.
void register_runtime_calendar(lv_obj_t* screen, lv_obj_t* body, lv_obj_t* left);

// ── Runtime state setters (called by the BLE layer) ────────────────────────

// Update whether the BLE PC link is currently up.
void set_pc_connected(bool connected);

// Mark that a full sync (SyncControl{END}) has just landed — the real
// "green flag" for the mode-toggle button, as opposed to merely being BLE
// connected (which set_pc_connected() above already tracks separately for
// weather/other uses). Call from handle_sync_end() only; every
// set_pc_connected() call — connect or disconnect — resets this back to
// false, so a fresh (re)connect hides the toggle again until it resyncs.
void set_pc_synced();

// Cache the most recent weather condition + temperature + unit + day/night +
// precipitation intensity pushed by Orion via the Device Settings
// characteristic ("w"/"d"/"u"/"n"/"i" fields — ble-protocol.md §3/§4, §6.4).
// Ephemeral: apply_widget_defaults() reflects it while the PC link is up and
// hides the icon/text entirely (no "unverified" state to fall back to) while
// it's down. unit is 0=Fahrenheit 1=Celsius (widget_profile_card::TemperatureUnit).
// is_night is passed straight through; intensity stays a raw uint8_t at this
// layer (widget_profile_card::WeatherIntensity) — same treatment as
// condition/unit, only widget_profile_card's own API uses the strongly-typed
// enums.
void set_weather(uint8_t condition, int16_t temp_f, uint8_t unit,
                  bool is_night, uint8_t intensity);

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

// User long-pressed the status-bar time — enter the Calendar (month view).
// Saves the current mode so on_mode_toggle() can return to it, same as
// on_clock_enter().
void on_calendar_enter();

// User tapped a meeting row in the list (screen_meeting_list.cpp). If `m`
// starts within the 5-minute countdown window (same window/logic as the
// automatic pre-meeting alert), shows the full-screen countdown modal for it
// — the same one the automatic alert uses — instead of the regular detail
// overlay, and returns true. Returns false (no-op) when the meeting isn't
// currently imminent, in which case the caller should fall back to the
// regular detail overlay.
bool show_countdown_if_imminent(const app_state::Meeting& m);

// The countdown modal was dismissed (Close tapped, or its own timer reached
// zero) — clears COUNTDOWN so the next tick's evaluate() resumes normal
// priority logic instead of staying wedged on COUNTDOWN forever (evaluate()
// only lets SETUP override it — see the COUNTDOWN case in state_machine.cpp).
// No-op if COUNTDOWN was already superseded by something else (OTA, factory
// reset) that set g_state directly. Called from modal_countdown.cpp.
void on_countdown_close();

// Query the current mode.
// Returns 0 = Calendar (meeting list), 1 = Media.
uint8_t current_mode();

// Clock-face preference (0 = Digital, 1 = Analog) — which screen
// build_clock_screen() shows for the CLOCK state. Persists immediately to
// NVS; safe to call from any non-LVGL-timer context (the ORI_DEBUG_SERIAL
// cycler, or an Orion-driven Device Settings BLE write).
void    set_clock_face(uint8_t face);
uint8_t current_clock_face();

// Time-format preference (0 = 24-hour, 1 = 12-hour). Persists to NVS via the
// time_format module and rebuilds the on-screen clock / meeting list so times
// re-render immediately. Safe to call from any non-LVGL-timer context (the
// Device Settings BLE write dispatch, the ORI_DEBUG_SERIAL cycler).
void    set_time_format(uint8_t fmt);

} // namespace state_machine
