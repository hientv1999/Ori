#pragma once

#include <lvgl.h>

#include "app_state.h"

// Ori — Reconnect-Syncing overlay.
//
// Per state-machine.md §Reconnect-Syncing: when BLE reconnects, Ori shows
// a progress ring + "Reconnecting to Orion…" / "Refreshing your day" copy
// OVER THE LEFT PANEL ONLY. Status bar and profile card stay visible.
//
// Ring shows sync progress (0–100 %) driven by OrioningProgress BLE events,
// matching the Orioning ring on the setup flow. set_progress() is called by
// ble_manager on each OrioningProgress event.

namespace screen_reconnect_syncing {

// Full-screen variant — builds a standalone screen (status bar + masked left
// panel + profile card). Used as the fallback when there's no live calendar
// screen to overlay onto (e.g. reconnecting while in Clock/Calendar/media).
lv_obj_t* create();

// In-place overlay variant — builds the sync ring as an opaque child that
// COVERS `left` (the live meeting/no-meetings screen's left panel), leaving
// the surrounding status bar and profile card completely untouched. This is
// what avoids the expensive full-screen teardown+rebuild on every reconnect
// sync (the status bar/profile card don't flash or re-layout). Returns the
// overlay object; remove it with destroy_overlay() (or by deleting `left`,
// which owns it). set_progress() drives whichever variant is on screen.
lv_obj_t* create_overlay(lv_obj_t* left);

// Removes the in-place overlay if present. Safe no-op if none is up (or if it
// was already removed by its parent left panel being rebuilt).
void destroy_overlay();

// Update the progress ring (0–100). No-op when no overlay/screen is up.
void set_progress(uint8_t pct);

} // namespace screen_reconnect_syncing
