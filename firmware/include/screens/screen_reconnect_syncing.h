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

lv_obj_t* create();

// Update the progress ring (0–100). No-op when the overlay is not on screen.
void set_progress(uint8_t pct);

} // namespace screen_reconnect_syncing
