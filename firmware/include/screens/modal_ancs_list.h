#pragma once

#include <stdint.h>
#include <lvgl.h>

// Ori — ANCS drill-down list.
//
// Reached by tapping a missed-call / unread-message / notification tile in
// the iPhone Info modal (modal_iphone_info). Shows one row per live
// notification GROUP in that bucket — notifications sharing the same
// (app token, title) collapse into a single row with a stacked count badge,
// the same rule ancs_client::list_bucket_groups() and
// app_state::ancs_collect_same_title() already use for the status bar and
// the detail overlay. Ports Ori_UI_Prototype.js's 'ancs-list' screen
// (SCREENS['ancs-list'], iphoneInfoHTML()'s stat() helper) to LVGL — see
// connectivity.md's "iPhone <-> Ori (ANCS)" tap-to-drill-down bullet and
// gestures.md for the authoritative interaction spec.
//
// LIVE while open — unlike modal_iphone_info/modal_unpair_phone (mostly
// snapshots), this list rebuilds itself whenever the underlying data
// actually changes: the ANCS filter changes (Device Settings "f", relayed
// through ancs_client::set_filter()), a new notification arrives, or one
// leaves — whether that removal came from the iPhone itself (an ANCS
// Removed event), from Orion (a char-0012 action write), from this list's
// own swipe-to-delete gesture, or from FIFO eviction at the 50-notification
// cap. See modal_ancs_list::refresh_active() below and ancs_client.cpp's
// g_ancs_list_refresh_pending for the mechanism (deferred by one main-loop
// tick for queue changes, to stay clear of the swipe gesture's own
// in-flight LVGL animation; immediate for filter changes, which carry no
// such hazard).
//
// Row tap     -> modal_ancs_notification::open_for_uid() layered ON TOP of
//                this modal's own scrim (this modal is NOT deleted first),
//                parented to the SAME base_screen this modal was opened
//                with — so the detail overlay renders as a later sibling
//                and stacks visually above the list; closing/reading it
//                reveals the list again underneath.
// Row swipe   -> left past ~35% of its own width commits: the row animates
//                fully off-screen + fades out, then every notification
//                sharing that row's group is cleared (ANCS negative action
//                where available, local drop otherwise) and the row is
//                deleted. Short of that threshold, it snaps back.
// Back button -> replaces this modal with modal_iphone_info (same
//                `connected` flag this modal was opened with).

namespace modal_ancs_list {

// bucket: ancs_client::ListBucket::MISSED / UNREAD / OTHER.
// connected: forwarded to modal_iphone_info::create() when Back is tapped —
// this modal itself doesn't otherwise need it (the tile that opens it is
// gated on `connected` alone by the caller — a zero-count tile still opens
// this modal, showing its empty state).
lv_obj_t* create(lv_obj_t* base_screen, uint8_t bucket, bool connected);

// Re-reads ancs_client::list_bucket_groups() for whichever bucket is
// currently open and rebuilds its rows, if any instance is open (no-op
// otherwise). Called from ancs_client::set_filter() directly, and from
// ancs_client::poll() when g_ancs_list_refresh_pending is set — see the
// module comment above.
void refresh_active();

} // namespace modal_ancs_list
