#pragma once

#include <lvgl.h>

// Ori — Time Off scenic screen.
//
// Left panel: full-bleed scenic with destination label + date range overlay.
// Status bar + profile card visible. Background priority: real BLE-pushed
// destination JPEG (photo_cache::get_time_off()) > compiled-in placeholder
// JPEG > painted gradient bands (used only if neither JPEG is available).

namespace screen_time_off {

lv_obj_t* create();

} // namespace screen_time_off
