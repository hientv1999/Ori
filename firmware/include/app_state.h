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
    const char* icons[MAX_ANCS_NOTIFICATIONS];  // full queue; only first MAX_ANCS_ICONS shown
    size_t      count;                           // live entries (≤ MAX_ANCS_NOTIFICATIONS)
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
    const char* body;          // message body (may be long — UI clips to 3 lines)
    const char* time_ago;      // relative timestamp ("2 min ago", "1 hr ago")
};

const AncsNotification& ancs_notification(const char* token);

} // namespace app_state
