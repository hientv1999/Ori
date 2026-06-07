#pragma once

#include <lvgl.h>

// Ori — Media mode screen.
//
// Left-panel layout, 528 × 396 px area:
//   1. Album-art image (240 × 240, centred horizontally)
//        - Tap                 → play/pause toggle
//        - Horizontal swipe    → prev / next track
//        - Vertical swipe      → volume, with momentary HUD overlay
//   2. Title + artist (centred below art, single-line ellipsis)
//   3. Three user-assignable icon-only shortcut buttons (bottom row)
//
// The album art slot shows the Ori brand mark on a dark gradient when
// app_state::media().has_media == false ("Nothing playing" state).
//
// Status bar + profile card are visible in this screen just like in
// calendar mode — only the left panel differs.

namespace screen_media_mode {

lv_obj_t* create();

// Live-update entry points — called by BLE handlers (M5) after writing to
// app_state so the visible screen reflects the new data immediately.
// Each function is a no-op when the media screen is not the active screen.
void update_meta(const char* title, const char* artist);
void update_playing(bool playing);
void update_seek(uint32_t position_s, uint32_t duration_s);

} // namespace screen_media_mode
