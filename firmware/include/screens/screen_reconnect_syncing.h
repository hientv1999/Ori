#pragma once

#include <lvgl.h>

#include "mock_data.h"

// Ori — Reconnect-Syncing overlay.
//
// Per state-machine.md §Reconnect-Syncing: when BLE reconnects, Ori shows
// a progress ring + "Reconnecting to Orion…" / "Refreshing your day" copy
// OVER THE LEFT PANEL ONLY. Status bar and profile card stay visible.
//
// Visually reuses the Step 3 "Orioning" ring at a slightly smaller size to
// fit inside the 528 px left panel cleanly. M3 builds the full screen
// (status + overlay + profile card). Auto-dismiss is M5.

namespace screen_reconnect_syncing {

lv_obj_t* create();

} // namespace screen_reconnect_syncing
