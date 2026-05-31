#pragma once

// Ori — M3 mock data.
//
// Mirrors the values in Ori_UI_Prototype.js (PROFILE, BLE_NAME, PASSKEY,
// TODAY_MEETINGS, OVERLAP_MEETINGS, LONG_TITLE_MEETINGS,
// OVERLAP_LONG_MEETINGS, LONG_LIST_MEETINGS). The frozen "now" is
// 14:30 Wed, May 14 — the value the prototype's status bar shows.
//
// Real data comes from NVS (M4) and BLE (M5). This module is the
// authoritative test-data source for M3 only.

#include <stdint.h>
#include <stddef.h>

namespace mock_data {

struct Profile {
    const char* name;
    const char* title;
    const char* initials;   // pre-derived for the photo placeholder
    const char* email;      // may be "" — not shown in overlay when empty
    const char* phone;      // may be "" — not shown in overlay when empty
};

struct Meeting {
    const char* start;
    const char* end;
    const char* title;
    const char* loc;
    const char* org;
    bool overlap;
    // True when start <= now <= end. Rendered with danger-red title + time
    // text so the user can spot the meeting they should currently be in
    // at a glance. M3 sets this statically in the mock; M4 will compute it
    // from the live clock vs each meeting's actual epoch start/end.
    bool in_progress;
};

struct MeetingList {
    const Meeting* items;
    size_t count;
};

struct Pto {
    const char* destination;
    const char* range_label;
};

struct Time {
    const char* hh_mm;        // "14:30"
    const char* date_short;   // "Wed, May 14"
    const char* date_long;    // "WEDNESDAY, MAY 14"
    const char* ampm;         // "PM"
    const char* hour_12;      // "2"
    const char* minute;       // "30"
};

// Stable getters — these never change at runtime.
const Profile&     profile();
const char*        ble_name();
const char*        passkey();
const Pto&         pto();
const Time&        now();

// Meeting variants — each maps to a prototype edge case.
MeetingList meetings();              // TODAY_MEETINGS
MeetingList meetings_overlap();      // OVERLAP_MEETINGS
MeetingList meetings_long_title();   // LONG_TITLE_MEETINGS
MeetingList meetings_overlap_long(); // OVERLAP_LONG_MEETINGS
MeetingList meetings_long_list();    // LONG_LIST_MEETINGS

// "SYNCED · 12 min ago" pill label for the cached-data variant.
const char* synced_pill_text();

// Now-playing media metadata for the Media mode screen.
// On hardware these come from Orion via BLE Media Metadata / Media Album
// Art characteristics. In M3 they're a static mock with a long title so
// we exercise the ellipsis-on-overflow rendering. Set has_media=false to
// preview the "Nothing playing" empty state.
struct Media {
    const char* title;
    const char* artist;
    bool        has_media;
    int         volume;      // 0..100 — mirrors Host Volume State characteristic
    uint32_t    position_s;  // playback position in seconds
    uint32_t    duration_s;  // total track duration in seconds
    bool        can_seek;    // mirrors MediaMetadata.can_seek — hides the timeline
                             // scrubber when false (app doesn't support OS seek API)
};
const Media& media();
void         set_media_playing(bool playing);   // toggles paused indicator
bool         media_playing();
void         set_media_volume(int v);

// Three user-assignable Media mode shortcut slots. Each slot has an
// icon-token string (matching the HTML prototype's KBD_SHORTCUTS). Default
// mock: mute audio, mute mic, screen capture.
struct ShortcutSlot {
    const char* icon_token;  // "vol-mute", "mic-mute", "screenshot", etc.
};
const ShortcutSlot* shortcuts();   // returns array of 3
constexpr size_t SHORTCUT_COUNT = 3;

// ANCS icon set + phone connectivity — settable so individual screens
// can override (e.g. the "phone disconnected" edge case).
//
// Icons are referenced by string token. M3 renders them as colored
// placeholders — real raster/vector assets land in M8.
// Two-tier notification capacity:
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
    const char* icons[MAX_ANCS_NOTIFICATIONS];  // full queue; only first MAX_ANCS_ICONS shown
    size_t      count;                           // live entries (≤ MAX_ANCS_NOTIFICATIONS)
    bool        phone_connected;
};

const AncsConfig& ancs_config();
void              set_ancs_config(const AncsConfig& cfg);

// Per-app ANCS notification data. In firmware these fields come from
// ANCS Notification Attribute commands (Title, Message, Date, DisplayName).
// Here they are static mocks so the detail modal can render without BLE.
struct AncsNotification {
    const char* display_name;  // human-readable app name ("Gmail", "Messenger")
    const char* title;         // notification title / sender name
    const char* body;          // message body (may be long — UI clips to 3 lines)
    const char* time_ago;      // relative timestamp ("2 min ago", "1 hr ago")
};

// Returns the mock notification for the given icon token (e.g. "gmail").
// Falls back to a generic placeholder if the token is not recognised.
const AncsNotification& ancs_notification(const char* token);

} // namespace mock_data
