#pragma once

#include <lvgl.h>

// Ori status bar — top 84 px, full panel width.
//
// Layout (left → right):
//   [date+time]    spacer    [ANCS icons]  [phone-disconnect?]  [mode-toggle?]
//
// The mode-toggle is the rightmost element and is **hidden when the PC
// link is down** (Controls mode is useless without Orion bridging). All
// elements pull from mock_data on refresh(). ANCS icons are colored
// 60 x 60 placeholder tiles in M3 — real raster assets land in M8.

namespace widget_status_bar {

constexpr int16_t HEIGHT = 84;

enum class Mode : uint8_t {
    Calendar = 0,  // shows the headphones icon (tap → enter Controls)
    Keyboard = 1,  // shows the calendar icon (tap → return to calendar)
};

// Callback fired when the user taps the mode-toggle. The screen manager
// uses this to switch between calendar and Controls screens.
using ModeToggleCb = void (*)(void);

lv_obj_t* create(lv_obj_t* parent);

// Toggle the left date/time block. The after-hours digital clock screen
// hides it because the screen itself shows the time.
void set_show_datetime(lv_obj_t* bar, bool show);

// Override phone connectivity for this specific instance. The phone-
// disconnect icon appears (and ANCS icons hide) when false.
void set_phone_connected(lv_obj_t* bar, bool connected);

// Record whether a phone BLE bond exists. Drives the choice between the
// re-pair screen (no bond) and the unpair modal (bond exists) on long-press.
// Does not change any visible UI on its own.
void set_phone_bonded(lv_obj_t* bar, bool bonded);

// PC (Orion) link state. When false, the mode-toggle button is removed
// from the status bar entirely — Controls mode is useless without the
// Orion bridge. Defaults to true.
void set_pc_connected(lv_obj_t* bar, bool connected);

// Which icon the mode-toggle shows. Calendar mode → headphones (tap to
// enter Controls); Keyboard mode → calendar icon with accent-tinted
// background (tap to return to calendar mode). Defaults to Calendar.
void set_mode(lv_obj_t* bar, Mode mode);

// Register a callback to receive mode-toggle taps. Pass nullptr to clear.
void set_mode_toggle_cb(lv_obj_t* bar, ModeToggleCb cb);

// Defaults applied to newly-created status bars. Set before calling
// create() so each new screen picks up the current state without the
// screen-manager having to remember to call set_pc_connected / set_mode
// on every load.
void set_default_pc_connected(bool connected);
void set_default_phone_bonded(bool bonded);
void set_default_mode(Mode mode);
void set_default_mode_toggle_cb(ModeToggleCb cb);

// Re-pull date / ANCS icons / phone state from mock_data.
void refresh(lv_obj_t* bar);

} // namespace widget_status_bar