#pragma once

#include <lvgl.h>

// Ori — iPhone Info/Stats overlay.
//
// Replaces the old direct-to-Unpair behaviour when tapping the status-bar
// phone icon while an iPhone is bonded: name, connection dot + live signal
// bars, and three icon+count tiles (missed calls, unread messages, and all
// other notifications — from ancs_client::phone_stats(), mutually exclusive
// counts). Cancel dismisses;
// Unpair hands off to the existing modal_unpair_phone confirmation, same
// two-step pattern as the Orion PC app's mirror of this screen.
//
// LIVE while open (2026-07-11 — previously a snapshot with one narrow
// filter-change exception; that limitation is gone, every value here now
// tracks the live state it's read from). Tapping a stat tile (missed calls
// / unread messages / notifications) while connected — even at zero count,
// see make_stat_unit's doc comment in the .cpp — opens modal_ancs_list, the
// on-device drill-down list for that one bucket (grouped by sender, tap a
// row for full detail, swipe left to clear). See modal_ancs_list.h.

namespace modal_iphone_info {

// connected: the caller (widget_status_bar) already tracks the iPhone link
// state authoritatively for the status-bar icon — passed in rather than
// re-derived here, since phone_stats() alone can't distinguish "connected
// with a genuinely empty/zero-signal reading" from "disconnected".
lv_obj_t* create(lv_obj_t* base_screen, bool connected);

// Re-reads ancs_client::phone_stats() and rebuilds the stat badges of the
// currently-open instance, if any (no-op otherwise). Called from
// ancs_client::set_filter() (a filter change) and from ancs_client's
// deferred queue-change drain (same trigger/timing as modal_ancs_list::
// refresh_active() — see that module's doc comment for why queue-driven
// refreshes are deferred by one tick while a filter change isn't).
// Connection state and signal bars are NOT touched here — see
// set_connected()/set_signal_bars() below.
void refresh_active();

// Updates the connection dot, status label, phone name (re-read fresh —
// title always reflects whichever iPhone is CURRENTLY connected, never a
// stale name cached from open), and rebuilds the stat badges (their
// clickability depends on `connected`, so a re-colour alone isn't enough).
// No-op if no instance is open. Called from state_machine::
// set_phone_connected() — the same authoritative choke-point
// widget_status_bar's own phone icon already trusts, fed by every iPhone
// connect/disconnect path.
void set_connected(bool connected);

// Recolours the 4 signal-strength bars in place (no full rebuild — this
// fires roughly every RSSI_POLL_INTERVAL_MS while connected, far more often
// than a filter/queue/connection change). No-op if no instance is open.
// Called from ancs_client::poll()'s periodic RSSI poll.
void set_signal_bars(uint8_t bars);

} // namespace modal_iphone_info
