#pragma once

#include <lvgl.h>

// Ori — Controls mode screen (internal name retains "keyboard_mode" /
// "kbd-mode" for historical reasons; user-facing copy says "Controls").
//
// Left-panel layout, 528 × 396 px area:
//   1. Album-art image (240 × 240, centred horizontally)
//        - Tap                 → play/pause toggle
//        - Horizontal swipe    → prev / next track
//        - Vertical swipe      → volume, with momentary HUD overlay
//   2. Title + artist (centred below art, single-line ellipsis)
//   3. Three user-assignable icon-only shortcut buttons (bottom row)
//
// In M3 the album art is a mock gradient (mirrors the HTML prototype's
// CSS gradient). When mock_data::media().has_media == false, the slot
// shows the Ori brand mark on a dark gradient instead.
//
// Status bar + profile card are visible in this screen just like in
// calendar mode — only the left panel differs.

namespace screen_keyboard_mode {

lv_obj_t* create();

} // namespace screen_keyboard_mode
