#pragma once

#include <lvgl.h>

// Ori status bar — top 84 px, full panel width.
//
// Layout (left → right):
//   [date+time]    spacer    [ANCS icons]  [phone-disconnect?]  [mode-toggle]
//
// The date+time block is tappable — fires the TimeTapCb to enter Clock mode.
// The mode-toggle cycles Calendar ↔ Media (2-mode cycle). It is hidden
// when the PC link is down — except in Clock mode, where it acts as a
// "return to previous mode" button and stays visible.
// All elements pull from app_state on refresh(). ANCS icons are colored
// 60 x 60 placeholder tiles in M3 — real raster assets land in M8.

namespace widget_status_bar {

constexpr int16_t HEIGHT = 84;

enum class Mode : uint8_t {
    Calendar = 0,  // toggle shows headphones glyph — "tap to enter Media"
    Clock    = 1,  // toggle shows calendar glyph   — "tap to return to previous mode"
    Keyboard = 2,  // toggle shows calendar glyph, accent-tinted — "tap to return to Calendar"
};

// Callback fired when the user taps the mode-toggle.
using ModeToggleCb = void (*)(void);

// Callback fired when the user taps the date+time block to enter Clock mode.
using TimeTapCb = void (*)(void);

lv_obj_t* create(lv_obj_t* parent);

// Toggle the left date/time block. The Clock screen hides it because the
// screen itself shows the time.
void set_show_datetime(lv_obj_t* bar, bool show);

// Override phone connectivity for this specific instance. The phone icon
// turns red (and ANCS icons hide) when false; neutral when true.
void set_phone_connected(lv_obj_t* bar, bool connected);

// Record whether a phone BLE bond exists. Drives the choice between the
// re-pair screen (no bond) and the unpair modal (bond exists) on long-press.
// Does not change any visible UI on its own.
void set_phone_bonded(lv_obj_t* bar, bool bonded);

// PC (Orion) link state. Updates the mode-toggle icon to reflect which mode
// comes next in the cycle (Controls is skipped when PC is offline).
void set_pc_connected(lv_obj_t* bar, bool connected);

// Which icon the mode-toggle shows — always the NEXT mode in the cycle.
// See Mode enum comments. Defaults to Calendar.
void set_mode(lv_obj_t* bar, Mode mode);

// Register a callback to receive mode-toggle taps. Pass nullptr to clear.
void set_mode_toggle_cb(lv_obj_t* bar, ModeToggleCb cb);

// Register a callback fired when the user taps the date+time block.
void set_time_tap_cb(lv_obj_t* bar, TimeTapCb cb);

// Defaults applied to newly-created status bars. Set before calling
// create() so each new screen picks up the current state without the
// screen-manager having to remember to call set_pc_connected / set_mode
// on every load.
void set_default_pc_connected(bool connected);
void set_default_phone_bonded(bool bonded);
void set_default_phone_connected(bool connected);
// Update both the default and the currently active bar's phone_bonded state.
void set_all_phone_bonded(bool bonded);
// Update both the default and the currently active bar's phone_connected
// state (recolours the phone icon + toggles ANCS-icon visibility). Driven
// by the authoritative BLE link signal, not the ANCS notification queue.
void set_all_phone_connected(bool connected);
void set_default_mode(Mode mode);
void set_default_mode_toggle_cb(ModeToggleCb cb);
void set_default_time_tap_cb(TimeTapCb cb);

// Re-pull date / ANCS icons / phone state from app_state.
void refresh(lv_obj_t* bar);

// Refresh the currently visible bar immediately. Call this whenever ANCS data
// changes so icons update without waiting for the 1-second clock timer.
void refresh_active();

// Mark a notification UID as "just arrived" so its icon plays a one-shot
// entrance animation on the next ANCS rebuild. Set by the ANCS client only for
// genuinely-new notifications (not the PreExisting backlog replayed on connect).
void note_new_notification(uint32_t uid);

} // namespace widget_status_bar