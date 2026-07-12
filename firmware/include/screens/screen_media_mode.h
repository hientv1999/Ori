#pragma once

#include <lvgl.h>

// Ori — Media mode screen.
//
// Left-panel layout, 528 × 396 px area:
//   1. Album-art image (484 × 216, full panel width)
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

// Refresh shortcut button icons from the current app_state::shortcuts() values.
// No-op when the media screen is not active.
void update_shortcuts();

// Album art — takes ownership of jpeg_buf (always freed internally).
// Decodes the JPEG to a PSRAM RGB565 buffer and shows it over the gradient
// fallback. Call with len == 0 to revert to the gradient (nothing playing).
void set_album_art(uint8_t* jpeg_buf, size_t len);
void clear_album_art();

} // namespace screen_media_mode
