#pragma once

#include <stdint.h>
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
// `uid` is the ANCS notification UID the tapped status-bar tile represents, so
// the overlay shows and acts on exactly that notification.
// "Read"  clears the notification on the iPhone (ANCS PerformNotificationAction
//         · Negative) and removes it from Ori's queue + status bar.
// "Close" deletes the modal only — the notification remains on both devices.
lv_obj_t* create(lv_obj_t* base_screen, uint32_t uid);

// Close the open detail overlay if it is currently showing notification `uid`.
// Called when the iPhone reports that notification was removed (ANCS Removed
// event), so a remotely-cleared notification doesn't leave a stale overlay up.
// No-op if no overlay is open or it's showing a different notification.
void close_if_showing(uint32_t uid);

} // namespace modal_ancs_notification
