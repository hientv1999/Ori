#pragma once

// Ori — global runtime state store.
//
// Holds shared data types and runtime state for subsystems that write from
// BLE handlers and read from UI screens: ANCS notification queue, media
// playback state, meeting/PTO types, and shortcut config.
// Real data is populated by NVS (M4) and BLE (M5); nothing here is mock.

#include <stdint.h>
#include <stddef.h>
#include <time.h>

namespace app_state {

struct Meeting {
    const char* start;
    const char* end;
    const char* title;
    const char* loc;
    const char* org;
    bool overlap;
    // True when start <= now <= end. Rendered with danger-red title + time.
    // Set by state_machine::filtered_meetings() from epoch times.
    bool in_progress;
};

struct MeetingList {
    const Meeting* items;
    size_t count;
};

// Returns "Ori-XX-XX" where XX-XX is derived from the device's base MAC
// (last 2 bytes, uppercase hex). Computed once and cached.
const char* ble_name();

// Records the wall-clock time of the most recent successful BLE sync.
// Call from gatt_server when SyncControl{op:"END"} is received.
void set_last_sync_time(time_t t);

// Returns "SYNCED · X min ago" / "SYNCED · X hr ago" based on the time
// recorded by set_last_sync_time(). Returns "SYNCED" before first sync.
const char* synced_pill_text();

// True once the wall clock has actually been set by an Orion Time Sync. After a
// cold power cycle there is no battery-backed RTC, so time() returns a ~1970
// value until the first sync — callers use this to avoid rendering a bogus time
// (show "--:--" instead). Threshold = 2026-07-12 (any later real epoch passes).
bool clock_is_set();

// Now-playing media metadata for the Media mode screen.
// Populated from Orion via BLE Media Metadata / Album Art characteristics.
// Starts with has_media=false ("Nothing playing" state) until a track arrives.
struct Media {
    const char* title;
    const char* artist;
    bool        has_media;
    int         volume;      // 0..100 — mirrors Host Volume State characteristic
    uint32_t    position_s;  // playback position in seconds — updated by BLE seek (M5)
    uint32_t    duration_s;  // total track duration in seconds — from MediaMetadata (M5)
    bool        can_seek;    // mirrors MediaMetadata.can_seek — hides the timeline
                             // scrubber when false (app doesn't support OS seek API)
};
const Media& media();
void         set_media_playing(bool playing);
bool         media_playing();
void         set_media_volume(int v);
void         set_media_meta(const char* title, const char* artist, bool can_seek);
void         set_media_seek(uint32_t position_s, uint32_t duration_s);

// Three user-assignable Media mode shortcut slots. Each slot has an
// icon-token string (matching the HTML prototype's KBD_SHORTCUTS). Default:
// mute audio, mute mic, screen capture.
struct ShortcutSlot {
    const char* icon_token;  // "vol-mute", "mic-mute", "screenshot", etc.
};
const ShortcutSlot* shortcuts();   // returns array of 3
constexpr size_t SHORTCUT_COUNT = 3;

// ANCS icon set + phone connectivity.
//
// MAX_ANCS_NOTIFICATIONS — internal queue depth. The ANCS handler (M5) tracks
//   up to this many live notifications. When a new one arrives and the queue is
//   full, the oldest entry is displaced (FIFO). When the user reads one (taps
//   the icon → detail modal → close), the entry is removed and the queue shifts
//   left, bringing the next hidden notification into the visible window.
//
// MAX_ANCS_ICONS — display cap. Status-bar layout budget:
//   bar inner width 776px minus datetime (~239px), mode-toggle (60px), and
//   3×16px column gaps = 429px available for the ANCS row.
//   Each slot costs 74px (60px tile + 14px gap), minus one trailing gap:
//     5 icons → 5×74−14 = 356px  (73px spacer remains)   ← safe
//     6 icons → 6×74−14 = 430px  (spacer negative, overflows) ← unsafe
//   refresh() always renders min(count, MAX_ANCS_ICONS) icons, so notifications
//   beyond the 5th are hidden but not lost — they become visible as earlier
//   ones are dismissed.
constexpr size_t MAX_ANCS_NOTIFICATIONS = 20;   // queue depth
constexpr size_t MAX_ANCS_ICONS         = 5;    // display cap (layout constraint)

struct AncsConfig {
    // Queue order is oldest → newest (index 0 = oldest, count-1 = newest). The
    // status bar shows the newest MAX_ANCS_ICONS, rightmost = newest.
    // Multiple notifications from the same app are stacked: one icon slot per
    // unique token, uid = most recent UID for that app, count = total stacked.
    const char* icons [MAX_ANCS_NOTIFICATIONS];  // app token per deduplicated slot
    uint32_t    uids  [MAX_ANCS_NOTIFICATIONS];  // most-recent UID per slot (parallel)
    uint8_t     counts[MAX_ANCS_NOTIFICATIONS];  // stacked notification count per slot
    size_t      count;                            // live slots (≤ MAX_ANCS_NOTIFICATIONS)
    bool        phone_connected;
};

const AncsConfig& ancs_config();
void              set_ancs_config(const AncsConfig& cfg);
// Remove the first queue entry matching token, shift remaining left.
// Called by the ANCS modal on "Read" so the status bar can reveal the next queued icon.
void              dismiss_ancs_notification(const char* token);

// ANCS notification content for the detail modal.
// Fields are populated from ANCS Notification Attribute commands (M5):
// Title, Message, Date, DisplayName. Returns a generic fallback until M5.
struct AncsNotification {
    const char* display_name;  // human-readable app name ("Gmail", "Messenger")
    const char* title;         // notification title / sender name
    const char* subtitle;      // ANCS Subtitle (e.g. mail subject, thread) — "" if none
    const char* body;          // message body (may be long — UI clips to 3 lines)
    const char* time_ago;      // relative timestamp ("2 min ago", "1 hr ago")
};

// ANCS notification categories (ble Notification Source CategoryID).
//   ACTIVE_CALL (12) is NOT in the original ANCS spec (which defines 0–11).
//   Modern iOS reports it for ongoing VoIP / CallKit calls (e.g. Viber): the
//   notification carries the caller name + a hang-up (negative) action and no
//   answer (positive) action. Treated as a call category so the in-call dialog
//   shows. See ancs_client.cpp call-overlay detection.
namespace AncsCategory {
    constexpr uint8_t OTHER = 0, INCOMING_CALL = 1, MISSED_CALL = 2, VOICEMAIL = 3,
                      SOCIAL = 4, SCHEDULE = 5, EMAIL = 6, NEWS = 7,
                      HEALTH = 8, FINANCE = 9, LOCATION = 10, ENTERTAINMENT = 11,
                      ACTIVE_CALL = 12;
}

const AncsNotification& ancs_notification(const char* token);

// Notification detail for a specific ANCS UID (each status-bar tile carries its
// UID, so reading a tile shows/dismisses exactly that notification — correct
// even when one app has several live notifications). Falls back to the generic
// placeholder if the UID has no stored detail.
const AncsNotification& ancs_notification_by_uid(uint32_t uid);

// Collect the UIDs of all stored notifications that share the given UID's app
// (token) AND title — i.e. the same conversation/sender — newest-first by
// arrival. Writes up to `max` UIDs to out_uids and returns the count (0 if the
// UID has no stored detail). The detail overlay uses this to stack same-title
// notifications into one screen and Read them all at once.
size_t ancs_collect_same_title(uint32_t uid, uint32_t* out_uids, size_t max);

// Store per-notification detail, pushed by the ANCS client after a
// GetNotificationAttributes response is parsed. Keyed by UID (so a Modified
// event updates in place and a Removed event can drop it); the detail modal
// looks entries up by icon token, with the most recently received one winning
// when an app has several live notifications.
//   recv_epoch — phone's notification time as a Unix epoch (0 = unknown)
//   hhmm       — TZ-free "HH:MM" from the ANCS Date attribute ("" = unknown)
void set_ancs_detail(uint32_t uid, const char* token, const char* display_name,
                     const char* title, const char* subtitle, const char* body,
                     time_t recv_epoch, const char* hhmm,
                     const char* bundle, uint8_t category, bool important);

// Update the stored display name for every notification from a given app
// bundle (used when GetAppAttributes resolves a previously-unknown app name).
void set_ancs_display_name_for_bundle(const char* bundle, const char* name);

// Category (AncsCategory::*) / importance for a stored notification UID, used
// by the status bar to accent incoming-call / important icons. 0 (OTHER) /
// false if the UID has no stored detail.
uint8_t ancs_category(uint32_t uid);
bool    ancs_is_important(uint32_t uid);

// Drop the stored detail for a notification UID (ANCS Removed event).
void remove_ancs_detail(uint32_t uid);

// UID of the notification the detail modal shows for this token (the most
// recently received one), or 0 if no detail is stored. Used to clear it on the
// iPhone via ANCS (PerformNotificationAction) when the user reads it.
uint32_t ancs_notification_uid(const char* token);

} // namespace app_state
