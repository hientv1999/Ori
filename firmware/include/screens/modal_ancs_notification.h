#pragma once

#include <lvgl.h>

namespace modal_ancs_notification {

// Creates a full-screen notification detail modal on top of `base_screen`.
// `token` is the ANCS app token (e.g. "gmail", "messenger") — used to look
// up the mock notification data and the app tile colour.
//
// Layout mirrors the meeting-detail overlay (text on scrim, no card box):
//   app icon tile (80×80)
//   notification title (font_title, primary)
//   message body    (font_meta, secondary, wrapping, up to ~3 lines)
//   timestamp       (font_meta, tertiary)
//   app name        (font_h2, primary)   ← anchor identifier, like the time
//                                           range in the meeting detail
//   [Read] [Close] buttons
//
// "Read"  removes the icon tile from the status bar ANCS row and deletes the
//         modal (simulates the ANCS PositiveAction → iOS Removed event path).
// "Close" deletes the modal only — the icon remains in the status bar.
lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ancs_tile, const char* token);

} // namespace modal_ancs_notification
