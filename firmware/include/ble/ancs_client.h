#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Ori ANCS client — Apple Notification Center Service subscriber.
//
// ANCS is Apple-proprietary. Ori acts as a GATT client and subscribes to:
//   - Notification Source (NS) characteristic: new / modified / removed events
//   - Data Source (DS) characteristic: notification attribute responses
//
// Rules (connectivity.md, firmware.md):
//   - Notification ICONS only — never content or counts displayed on the icon
//   - Tapping an icon fires an ANCS GetNotificationAttributes request for the
//     Title and Body, which are shown in modal_ancs_notification
//   - MAX_ANCS_NOTIFICATIONS = 100 queue depth (PSRAM-backed detail store);
//     MAX_ANCS_ICONS = 5 visible
//
// This module also handles the BLE connection setup for the iPhone link
// (separate from the Orion link).

namespace ancs_client {

// Called once from ble_manager::init() — registers ANCS GATT client profiles
// with NimBLE. Does not start connection (that happens in advertising callbacks).
void init();

// Drain queued NS/DS notifications on the MAIN task. The NimBLE notify
// callbacks run on the host task and only enqueue raw bytes; this does the
// actual work (attribute requests, queue + status-bar updates), where blocking
// GATT writes and LVGL calls are safe. Call once per main-loop iteration.
// `orion_connected` gates the periodic iPhone CTS time-sync (skipped when
// Orion is connected, since Orion's own 10-min Time Sync takes priority).
void poll(bool orion_connected);

// Called when the iPhone BLE link is established and services are discovered.
// Subscribes to NS and DS characteristics.
void on_iphone_connected(uint16_t conn_handle);

// Called when iPhone disconnects — clears queue, notifies state machine.
// Deliberately does NOT clear the cached phone_name()/phone_device_type() —
// see clear_phone_identity() below for why and where those actually get wiped.
void on_iphone_disconnected();

// Wipes the cached phone_name()/phone_device_type() — called only when the
// iPhone BOND itself is deleted (ble_manager::finish_iphone_bond_wipe(), the
// single choke-point both the local Unpair button and a remote Orion unpair
// command funnel through), never on a plain disconnect. A device's name/model
// doesn't change for a given bond, so on_iphone_disconnected() intentionally
// keeps showing the last-known value across a runtime disconnect (same "don't
// blank what's still true" treatment Orion gives its own cached copy of
// device_type, ble-protocol.md's PhoneBondStatus.d) — only a genuine unpair
// (or the factory reset that implicitly reboots and drops it anyway) should
// clear it.
void clear_phone_identity();

// ── Firmware-transfer suspension (ota.md "Behaviour") ──────────────────────
// While a firmware download is streaming into PSRAM over BLE, ANCS processing
// is suspended at the source: the NS/DS notify callbacks drop incoming events
// instead of ringing them (per-notification attribute fetches + icon lookups
// across the 48-app registry are the heaviest BLE-triggered work in the
// system — firmware.md — and now compete for the very same radio carrying the
// image), and poll() skips its whole body (CTS read, RSSI HCI round-trip,
// NS/DS drain). ble_manager then drops the phone link outright on top of this.
// suspend_for_ota() is called from ota_receiver's accepted-BEGIN path via
// ble_manager::set_ota_transfer_quiet(true).
void suspend_for_ota();

// Lifts the suspension (failure-resume path — a successful commit reboots
// instead). Returns true if any NS/DS event was actually dropped while
// suspended: iOS never re-sends missed events on the same connection, so the
// caller (ble_manager::set_ota_transfer_quiet(false)) force-drops the iPhone
// link in that case — the bonded auto-reconnect replays the full backlog
// (the same "PreExisting replay on the NEXT connection" behavior every fresh
// bond already relies on, setup-flow.md). Returns false when nothing was
// missed, so the common quick-failure case skips the reconnect entirely.
bool resume_from_ota_suspend();

// Called from NimBLE notification callback with raw NS data (8 bytes).
void on_notification_source(const uint8_t* data, uint16_t len);

// Called from NimBLE notification callback with raw DS data (attribute response).
void on_data_source(const uint8_t* data, uint16_t len);

// Request attributes (Title, Body) for a given notification UID.
// Posts the ANCS GetNotificationAttributes command over BLE.
void request_attributes(uint32_t notif_uid);

// Dismiss a notification from the live queue by UID and send ANCS
// PerformNotificationAction(Negative) to clear it on the phone.
// Only call when the notification has a negative action (has_neg_action = true).
void dismiss_notification(uint32_t notif_uid);

// Remove a notification from Ori's queue without sending any ANCS action to
// the phone — used when the notification has no negative action available.
void drop_notification(uint32_t notif_uid);

// Accept/answer a notification's positive action via ANCS
// PerformNotificationAction(Positive) — used to answer an incoming call. Does
// NOT remove it from the queue: the call becomes active and stays until it ends.
void answer_notification(uint32_t notif_uid);

// Returns the current live queue as an array of icon tokens.
// Only the first MAX_ANCS_ICONS entries are displayed; the full queue is
// tracked up to MAX_ANCS_NOTIFICATIONS entries.
struct QueueEntry {
    uint32_t    uid;
    char        icon_token[32];
};

// The connected iPhone's device name (GAP Device Name characteristic,
// 0x1800/0x2A00 — e.g. "Xander's iPhone"). Read once per connection over
// the encrypted link; iOS only reveals the personalised name to bonded
// peers. RAM only, but survives a runtime disconnect — keeps showing the
// last-known value (rather than "") until the bond is actually unpaired
// (clear_phone_identity() above), same reasoning Orion applies to its own
// cached device_type. Empty "" only before the very first successful read
// of a fresh bond, or after an unpair.
const char* phone_name();

// The connected iPhone's model (Device Information Service, 0x180A / Model
// Number String 0x2A24 — e.g. "iPhone 15 Pro"). Read once per connection,
// same encrypted-link/bonded-peers treatment, and the same disconnect-
// survives-until-unpair caching, as phone_name() above.
const char* phone_device_type();

// "iPhone" or "iPad" once phone_device_type() is known — Ori bonds with
// either in the same slot (connectivity.md), and Apple's own naming means
// every resolved/raw device_type string already starts with one or the
// other, so this is a plain prefix check, no extra state. Before the very
// first successful model read of a fresh bond (device_type still ""),
// returns the generic "iPhone or iPad" instead of guessing — the same
// wording used pre-bond (setup Step 3 / the runtime re-pair screen). Lets UI
// text (modal_iphone_info.cpp, modal_unpair_phone.cpp) say the right noun
// without hardcoding "iPhone".
const char* phone_kind_word();

// Snapshot of the iPhone's live notification stats + link signal, for the
// on-device iPhone Info/Stats overlay (modal_iphone_info) and the Phone Bond
// Status BLE characteristic that relays the same values to Orion. Computed
// fresh from the live ANCS queue on every call (cheap — at most
// MAX_ANCS_NOTIFICATIONS entries):
//   missed      — CategoryID MISSED_CALL
//   unread      — CategoryID SOCIAL (ANCS' closest category to "message"
//                 apps — Messages/WhatsApp/iMessage etc. all report Social)
//   total       — every currently-active notification in every OTHER
//                 category — excludes whatever's already counted in missed/
//                 unread so the three counts are mutually exclusive (a
//                 missed call isn't double-counted under this one too)
//   signal_bars — 0-4, bucketed from the live connection RSSI
//                 (ble_gap_conn_rssi via NimBLEClient::getRssi())
//   battery     — 0-100 (%), from the Battery Service (0x180F / Battery
//                 Level 0x2A19). Read once on connect, then notify-driven
//                 (subscribe_phone_battery() in the .cpp) — no polling.
// The three counts are FILTER-GATED — only notifications passing the current
// ancs_filter count — and exclude ringing/active calls (those have their own
// live call UI, never a badge or list row). Same single counting rule as
// Orion's relayed copy and the drill-down lists (count_filtered_stats in the
// .cpp), so a tile badge always matches the list a tap on it opens.
// All fields are 0 while the iPhone isn't connected — nothing to verify.
struct PhoneStats {
    uint8_t missed;
    uint8_t unread;
    uint8_t total;
    uint8_t signal_bars;
    uint8_t battery;   // 0-100 (%); 0 while disconnected or unread
};
PhoneStats phone_stats();

// One deduplicated row for the on-device ANCS drill-down list
// (modal_ancs_list, built alongside this change) — notifications sharing the
// same (app token, title) collapse into one group, same rule as
// app_state::ancs_collect_same_title() / the status bar's AncsConfig.
struct ListGroup {
    uint32_t uid;             // most-recent uid in the group — the
                              // representative notification the row shows
                              // and hands to modal_ancs_notification::open_for_uid()
                              // on tap, and to app_state::ancs_collect_same_title()
                              // to resolve the full group for a swipe-delete.
    uint8_t  count;            // number of live notifications collapsed into this group
    char     icon_token[32];   // same size/meaning as QueueEntry::icon_token
};

// Bucket a drill-down list shows — mirrors PhoneStats' three mutually
// exclusive counts (missed calls / unread messages / everything else).
namespace ListBucket {
    constexpr uint8_t MISSED = 0, UNREAD = 1, OTHER = 2;
}

// Enumerate every live notification GROUP whose category falls in `bucket`,
// newest-group-first, deduplicated by (app token, title) via
// app_state::ancs_collect_same_title() (the same stacking primitive
// modal_ancs_notification's detail overlay already uses — not reimplemented
// here). Group order mirrors publish_queue()'s own representative-recency
// convention for the status bar (recv_epoch when known on both sides, else
// g_queue arrival-index as the tiebreak/fallback) so the drill-down list and
// the status-bar icons never disagree on ordering for the same underlying
// queue — just walked newest-first here instead of publish_queue()'s
// oldest-first (it builds left→right, rightmost = newest; this list wants
// newest first). Ringing/active calls (AncsCategory::INCOMING_CALL /
// ACTIVE_CALL) are always excluded from every bucket regardless of `bucket`
// — they have their own live modal_incoming_call UI, not a static list row.
// FILTER-GATED: only notifications passing the current ancs_filter appear
// (a group counts/represents its passing members only; all-filtered groups
// produce no row) — same filter the status bar, tile badges (phone_stats),
// and Orion relay honor, so the badge count always matches this list.
// (Policy changed 2026-07-11; previously the drill-down deliberately
// bypassed the filter.)
// Writes up to `max` groups to `out`; returns the count written.
size_t list_bucket_groups(uint8_t bucket, ListGroup* out, size_t max);

// ANCS notification filter level. Applied at DISPLAY TIME — all notifications are
// always stored internally so changing the filter instantly shows/hides them.
//   0x00  DISABLED         — store all; display none (no icons, no call modal, no badge animation)
//   0x01  CALL_ONLY        — display only CategoryID 1 (IncomingCall)
//   0x02  IMPORTANT        — display IncomingCall OR ANCS Important flag set
//   0x03  APP_PASSTHROUGH  — display IncomingCall OR any app on the allowlist
//                            set by set_app_filter() below
//   0x04  ALL              — display all (default)
// Ordered narrowest to widest, matching the order Orion lists them in, so the
// value doubles as the list position on both sides.
// set_filter() is called from ble_manager::poll() after Orion writes Device Settings (char 000E);
// the value is also persisted to NVS (nvs::set_notif_filter) by that poll handler.
// set_filter() immediately calls publish_queue() to refresh the status bar.
void set_filter(uint8_t level);

// Largest AppPassthrough allowlist Ori will hold, in packed bytes (each token
// stored NUL-terminated back to back). Comfortably above the worst real case —
// every one of the ~49 compiled-in ancs_icons.h brands selected at once is
// around 550 bytes — with room for the token vocabulary to keep growing.
constexpr size_t ANCS_APP_FILTER_MAX_BYTES = 1024;

// AppPassthrough allowlist (filter level 0x03): `packed` is a run of
// NUL-terminated ancs_icons.h tokens ("slack\0teams\0gmail\0") and `len` its
// total byte count including terminators — the same layout nvs::get_ancs_apps()
// returns and the ANCS App Filter characteristic carries on the wire
// (ble-protocol.md §4). Anything over ANCS_APP_FILTER_MAX_BYTES is rejected
// whole rather than truncated mid-token. Passing len == 0 clears the list,
// which makes level 0x03 pass calls only.
//
// Independent of set_filter(): the list can be set while another level is
// active (it just has no effect until the level becomes 0x03), and changing
// either one re-runs the same publish/relay/badge refresh set_filter() does,
// so every surface re-evaluates at once. Called from ble_manager::poll();
// persisted to NVS (nvs::set_ancs_apps) by that same poll handler.
void set_app_filter(const char* packed, size_t len);

// Full clear-and-repopulate of Orion's ANCS mirror (char 0010) from the
// current queue, filtered through the current filter level the same way a live
// relay_ancs_add() call would be. Called from set_filter() (the filtered SET
// changed) and from ble_manager's BleEventType::AncsResubscribed handler,
// fired by gatt_server.cpp's onSubscribe() the moment Orion's own CCCD write
// for char 0010 lands (char 0010 has no read property and no
// reconnect-replay of its own — unlike PhoneBondStatus, char 000F — so this
// is the only way a notification added while Orion was disconnected ever
// reaches its local mirror). Deliberately NOT triggered by OrionConnected —
// that event fires on encryption complete, well before Orion has even
// started run_sync, let alone subscribed to this characteristic; a resync
// sent that early is silently dropped. See the .cpp doc comment for the
// full failure mode this closes.
void resync_orion_relay();

// Replays the current call state to Orion (char 0011) — the AncsCallState
// counterpart to resync_orion_relay() above, triggered the same way (char
// 0011's own onSubscribe, not OrionConnected) for the same reason: char
// 0011 is notify-only with no replay of its own, so a call already
// ringing/active before Orion connects would otherwise never raise Orion's
// incoming/in-call view for it. See the .cpp doc comment.
void resync_orion_call_state();

// True if `uid` is currently a live entry in Ori's ANCS queue. Used by
// ble_manager::poll() to validate an incoming ANCS Notification Action (char
// 0012, ble-protocol.md §13) before dispatching answer_notification()/
// dismiss_notification() below — a uid that's already been removed (the
// notification was cleared, dismissed, or evicted before Orion's write
// landed) gets NACK_CBOR_DECODE via SyncControl instead of acting on a stale
// target. Cheap linear scan (≤ MAX_ANCS_NOTIFICATIONS entries); call only
// from the main task, same as the rest of this module's queue-touching API.
bool is_queued(uint32_t uid);

} // namespace ancs_client
