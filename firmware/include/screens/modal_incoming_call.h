#pragma once

#include <stdint.h>
#include <lvgl.h>

// Ori — Incoming-call banner.
//
// Shown over the active screen when an ANCS notification arrives with
// CategoryID = IncomingCall (and it isn't part of the reconnect backlog). Pure
// awareness + the ANCS Negative action: "Decline" clears the call on the
// iPhone; "Dismiss" only hides the banner (the call keeps ringing). There is no
// "Answer" — Ori has no audio path (see product-intent.md). Auto-closes when
// the call notification is removed (answered/ended on the phone).

namespace modal_incoming_call {

// Raise the banner for ANCS notification `uid`. Replaces any banner already up.
void show(uint32_t uid);

// Close the banner if it is currently showing `uid` (ANCS Removed for it).
void close_if_showing(uint32_t uid);

} // namespace modal_incoming_call
