#pragma once

#include <lvgl.h>

// Ori — Media mode screen.
//
// Left-panel layout, 528 × 396 px area:
//   1. Album-art image (484 × 216, full panel width) — YouTube-style
//      touch-revealed controls (media-mode.md):
//        - A plain tap             → resolved only after DOUBLE_TAP_MS with
//                                 no follow-up tap in the same half (so it
//                                 isn't mistaken for half of a double-tap):
//                                 reveals if the controls started hidden,
//                                 dismisses (skipping the 5s wait) if they
//                                 were already showing. Full brightness / no
//                                 overlay by default; always auto-hides
//                                 after 5s idle otherwise, whether playing
//                                 or paused — no permanently-visible state.
//                                 Button + progress bar fade out together
//                                 (~400ms) on dismiss/timeout; the scrim
//                                 snaps instantly both ways.
//        - Swipe (horizontal or vertical) → does NOT touch the button/bar
//                                 controls' visibility at all — shown stays
//                                 shown, hidden stays hidden. Only the
//                                 (separate) volume HUD reacts to a
//                                 vertical swipe.
//        - Tap the button       → play/pause toggle (the only way to toggle
//                                 locally); an externally-driven (Orion)
//                                 play/pause change never reveals or hides
//                                 the controls itself
//        - Double-tap left/right half → seek backward/forward; NEVER
//                                 reveals the button/bar if they started
//                                 hidden (only extends the countdown if
//                                 they were already visible) — the seek
//                                 flash is its own independent feedback,
//                                 unaffected by the button/bar's visibility
//        - New media metadata, album art, or total playtime changes →
//                                 reveals the controls, but only for a
//                                 genuine change: title actually differs
//                                 (metadata), a new image finishes decoding
//                                 or art is explicitly cleared (album art),
//                                 or duration_s actually differs from
//                                 what's tracked (total playtime) — an
//                                 Orion play/pause echo (same title, same
//                                 duration) must NOT reveal them. A play
//                                 POSITION change alone NEVER reveals —
//                                 position_s is excluded from the
//                                 comparison entirely, whether it's Ori's
//                                 own internal per-second dead-reckoning
//                                 tick, Ori's own local drag-/double-tap-
//                                 seek gestures, or a genuine seek on Orion
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

// Loading indicator shown over the art while Orion streams a new Media Album
// Art transfer — a dim (not opaque) black veil covers the entire art at 0%
// progress (the default/current art reads as dimmed, still dimly visible
// underneath) and is revealed to full opacity from the bottom edge upward as
// the transfer progresses, fully gone (full opacity/brightness) at 100%,
// right before the real decoded image swaps in. See the on_fragment/on_complete
// wiring in gatt_server.cpp's handle_album_art(). show_art_loading() is
// called once the first chunk fragment arrives (never just on entering
// Controls mode), starting the veil at full height. update_art_loading_progress()
// is called on every subsequent fragment with the current percent (0-99),
// computed from seq/total_frags, shrinking the veil's (top-anchored) height
// to (100-pct)% of the art's height. hide_art_loading() is called on
// completion, decode failure, or NACK — set_album_art() and clear_album_art()
// already call it, so callers only need it for the NACK path.
void show_art_loading();
void update_art_loading_progress(uint8_t pct);
void hide_art_loading();

// Host Volume State (char 000B) pushed from Orion — always refreshes the HUD
// fill/percentage so it's never stale, and when `show_toast` is true (a
// genuine externally-driven change, e.g. the user adjusted volume via the OS
// mixer or another app — as opposed to Orion's own echo of a swipe Ori just
// made) also surfaces the HUD if it isn't already showing, auto-hiding again
// after a few seconds. No-op when the media screen isn't active, and never
// fights an in-progress local swipe (which already owns the HUD).
void update_volume_from_host(uint8_t level, bool show_toast);

} // namespace screen_media_mode
