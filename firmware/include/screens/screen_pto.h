#pragma once

#include <lvgl.h>

// Ori — PTO scenic screen.
//
// Left panel: full-bleed scenic with destination label + date range overlay.
// Status bar + profile card visible. M3 renders the scene as a gradient
// approximation (no JPEG decoder available) — real photo lands in M8 when
// SJPG decoding + asset packing land.

namespace screen_pto {

lv_obj_t* create();

} // namespace screen_pto
