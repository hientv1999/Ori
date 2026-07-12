#pragma once

#include <stdint.h>
#include <lvgl.h>

namespace modal_ancs_notification {

// Creates a full-screen notification detail modal on top of `base_screen` for
// ANCS notification `uid`. Notifications sharing the same (app, title) are
// stacked into one overlay with a single "Read all" action — see the .cpp's
// module comment for the full layout and button rules (buttons are dynamic,
// driven by the notification's ANCS EventFlags, not a fixed [Read][Close] pair).
lv_obj_t* create(lv_obj_t* base_screen, uint32_t uid);

// Shared tap-to-view entry point for any ANCS notification UID — routes to
// modal_incoming_call::show_active() when the notification is a still-
// ringing/active call (CategoryID IncomingCall/ActiveCall), or this module's
// own read-only detail overlay (create(), above) for everything else. Used
// by every place a user can tap a specific notification: the status-bar
// icon tiles and the iPhone Info modal's drill-down lists.
void open_for_uid(lv_obj_t* base_screen, uint32_t uid);

// Close the open detail overlay if it is currently showing notification `uid`.
// Called when the iPhone reports that notification was removed (ANCS Removed
// event), so a remotely-cleared notification doesn't leave a stale overlay up.
// No-op if no overlay is open or it's showing a different notification.
void close_if_showing(uint32_t uid);

} // namespace modal_ancs_notification
