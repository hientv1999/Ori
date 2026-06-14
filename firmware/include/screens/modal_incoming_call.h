#pragma once

#include <stdint.h>
#include <lvgl.h>

// Ori — Incoming-call banner / in-call dialog.
//
// Shown over the active screen when an ANCS notification arrives with
// CategoryID = IncomingCall (and it isn't part of the reconnect backlog).
//
//  • Ringing  (show):        "Decline" sends the ANCS Negative action (clears
//                            the call on the iPhone); "Dismiss" only hides the
//                            banner (the call keeps ringing). No "Answer" — Ori
//                            has no audio path, you answer on the phone.
//  • On call  (show_active): once the call is answered on the phone the ANCS
//                            notification stops offering the positive (answer)
//                            action; ancs_client detects that and swaps to this
//                            in-call dialog — caller name + a live duration
//                            timer + "End call" (ANCS Negative = hang up).
//
// Both auto-close when the call notification is removed (ended on the phone).

namespace modal_incoming_call {

// Raise the ringing banner for ANCS notification `uid`. Replaces any banner up.
void show(uint32_t uid);

// Auto entry: the call `uid` is active (answered). Starts the duration session
// (which keeps running while the dialog is hidden) and auto-presents the in-call
// dialog once. Called by the ANCS client; won't re-pop a dialog the user hid.
void notify_active(uint32_t uid);

// Explicit open of the in-call dialog (caller + live timer + End call) for
// `uid` — e.g. tapping the call's status-bar icon. Reopens with the running
// duration; never resets the timer.
void show_active(uint32_t uid);

// Close the banner/dialog if it is currently showing `uid`, and stop the call
// duration timer (ANCS Removed = the call ended).
void close_if_showing(uint32_t uid);

// Force-close any open call banner/dialog and stop the duration timer,
// regardless of UID. Call when the iPhone (ANCS) link drops — the call can't
// continue without it, so a stale dialog/timer would lie about reality.
void close_all();

} // namespace modal_incoming_call
