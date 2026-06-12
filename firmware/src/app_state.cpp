#include "app_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_mac.h>

namespace app_state {

namespace {

// Generic fallback for the ANCS notification detail modal — used only before a
// notification's attributes have arrived. Empty time_ago so no fake time shows.
static const AncsNotification k_ancs_fallback = {
    "Notification", "Notification", "", "No preview available.", ""
};

// ── ANCS notification detail store ─────────────────────────────────────────
// Populated by the ANCS client via set_ancs_detail() after a
// GetNotificationAttributes response is parsed. Sized to the queue depth so
// even the hidden (beyond the 5 visible) notifications keep their content.
struct AncsDetailEntry {
    uint32_t uid;
    char     token[32];
    char     bundle[48];     // app bundle id — for retroactive display-name fill
    char     display_name[40];
    char     title[193];
    char     subtitle[129];  // ANCS Subtitle (mail subject / thread); "" if none
    char     body[257];
    time_t   recv_epoch;     // phone's notification time; 0 = unknown
    char     hhmm[6];        // "HH:MM" from ANCS Date (TZ-free); "" = unknown
    char     time_ago[24];   // formatted on lookup (fresh when the modal opens)
    uint8_t  category;       // AncsCategory::*
    bool     important;      // EventFlags Important bit
    uint32_t seq;            // insertion order — higher = more recent
    bool     used;
};
static AncsDetailEntry  k_detail[MAX_ANCS_NOTIFICATIONS] = {};
static uint32_t         k_detail_seq = 0;
static AncsNotification k_detail_view;  // const-char* view returned by ancs_notification()

// Runtime ANCS state — starts empty (no phone connected). Populated by the
// real ANCS client via set_ancs_config() when the iPhone connects.
AncsConfig k_ancs = {
    {},
    0,
    false,
};

// Runtime media state. Starts empty — populated by BLE MediaMetadata writes.
static char k_media_title[193] = {};
static char k_media_artist[97] = {};
Media k_media = {
    k_media_title,
    k_media_artist,
    /*has_media=*/false,
    /*volume=*/0,
    /*position_s=*/0,
    /*duration_s=*/0,
    /*can_seek=*/false,
};
bool k_playing = false;

// Default shortcut config.
const ShortcutSlot k_shortcuts[SHORTCUT_COUNT] = {
    { "vol-mute"   },
    { "mic-mute"   },
    { "screenshot" },
};

// Wall-clock time of the last successful BLE sync (SyncControl END).
// Zero until the first sync completes.
time_t g_last_sync_epoch = 0;

} // namespace

const char* ble_name() {
    // Canonical Ori BLE name — single source of truth for both the advertised
    // name (ble_manager) and the on-screen pairing pill. MUST use the BT MAC
    // (ESP_MAC_BT): that's the address the BLE stack actually advertises, so it's
    // what the iPhone/Orion see. The WiFi STA MAC differs (BT MAC = base + 2 on
    // the last octet), which previously made the screen and the advert disagree.
    static char buf[12] = {};
    if (buf[0]) return buf;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(buf, sizeof(buf), "Ori-%02X-%02X", mac[4], mac[5]);
    return buf;
}

void set_last_sync_time(time_t t) {
    g_last_sync_epoch = t;
}

const char* synced_pill_text() {
    static char buf[36];
    if (g_last_sync_epoch == 0) return "SYNCED";
    time_t elapsed = time(nullptr) - g_last_sync_epoch;
    if (elapsed < 60)
        snprintf(buf, sizeof(buf), "SYNCED \xc2\xb7 just now");
    else if (elapsed < 3600)
        snprintf(buf, sizeof(buf), "SYNCED \xc2\xb7 %ld min ago", (long)(elapsed / 60));
    else
        snprintf(buf, sizeof(buf), "SYNCED \xc2\xb7 %ld hr ago",  (long)(elapsed / 3600));
    return buf;
}

const AncsConfig& ancs_config() { return k_ancs; }

void set_ancs_config(const AncsConfig& cfg) {
    k_ancs = cfg;
    if (k_ancs.count > MAX_ANCS_NOTIFICATIONS) k_ancs.count = MAX_ANCS_NOTIFICATIONS;
}

// Find the most recently received detail entry for an icon token (nullptr = none).
static AncsDetailEntry* newest_detail_for_token(const char* token) {
    if (!token) return nullptr;
    AncsDetailEntry* best = nullptr;
    for (auto& e : k_detail) {
        if (e.used && strcmp(e.token, token) == 0 && (!best || e.seq > best->seq))
            best = &e;
    }
    return best;
}

void dismiss_ancs_notification(const char* token) {
    if (!token) return;
    // Drop the detail entry the modal was showing (the newest for this token),
    // so a re-tap on another instance of the same app doesn't show stale text.
    if (AncsDetailEntry* d = newest_detail_for_token(token)) d->used = false;

    for (size_t i = 0; i < k_ancs.count; ++i) {
        if (k_ancs.icons[i] && strcmp(k_ancs.icons[i], token) == 0) {
            for (size_t j = i; j + 1 < k_ancs.count; ++j)
                k_ancs.icons[j] = k_ancs.icons[j + 1];
            k_ancs.icons[--k_ancs.count] = nullptr;
            return;
        }
    }
}

void set_ancs_detail(uint32_t uid, const char* token, const char* display_name,
                     const char* title, const char* subtitle, const char* body,
                     time_t recv_epoch, const char* hhmm,
                     const char* bundle, uint8_t category, bool important) {
    // Reuse the slot for this uid (Modified), else a free slot, else evict the
    // oldest (lowest seq) so a full queue keeps the most recent detail.
    AncsDetailEntry* slot = nullptr;
    for (auto& e : k_detail) if (e.used && e.uid == uid) { slot = &e; break; }
    if (!slot) for (auto& e : k_detail) if (!e.used) { slot = &e; break; }
    if (!slot) { slot = &k_detail[0]; for (auto& e : k_detail) if (e.seq < slot->seq) slot = &e; }

    slot->used       = true;
    slot->uid        = uid;
    slot->seq        = ++k_detail_seq;
    slot->recv_epoch = recv_epoch;
    slot->category   = category;
    slot->important  = important;
    snprintf(slot->token,        sizeof(slot->token),        "%s", token ? token : "unknown");
    snprintf(slot->bundle,       sizeof(slot->bundle),       "%s", bundle ? bundle : "");
    snprintf(slot->display_name, sizeof(slot->display_name), "%s",
             (display_name && display_name[0]) ? display_name : "Notification");
    snprintf(slot->title,        sizeof(slot->title),        "%s", title    ? title    : "");
    snprintf(slot->subtitle,     sizeof(slot->subtitle),     "%s", subtitle ? subtitle : "");
    snprintf(slot->body,         sizeof(slot->body),         "%s", body     ? body     : "");
    snprintf(slot->hhmm,         sizeof(slot->hhmm),         "%s", hhmm     ? hhmm     : "");
}

void set_ancs_display_name_for_bundle(const char* bundle, const char* name) {
    if (!bundle || !bundle[0] || !name || !name[0]) return;
    for (auto& e : k_detail) {
        if (e.used && strcmp(e.bundle, bundle) == 0)
            snprintf(e.display_name, sizeof(e.display_name), "%s", name);
    }
}

uint8_t ancs_category(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) return e.category;
    return AncsCategory::OTHER;
}

bool ancs_is_important(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) return e.important;
    return false;
}

void remove_ancs_detail(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) { e.used = false; return; }
}

uint32_t ancs_notification_uid(const char* token) {
    AncsDetailEntry* d = newest_detail_for_token(token);
    return d ? d->uid : 0;
}

namespace {

static const char* const kWeekday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const kMonth[12]  = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Civil date → days since 1970-01-01 (Howard Hinnant's algorithm), so we can
// compare two instants by whole *local calendar day* rather than raw seconds.
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    int  yoe = (int)(y - era * 400);
    int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = (long)yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Apple-style notification timestamp. iOS shows a relative time for the first
// ~4 hours, then switches to absolute, then to relative *days* (Yesterday →
// weekday → date) — see Volect, "When Did You Really Get That Notification?".
//   < 1m            → "now"
//   < 1h            → "5m ago"
//   < 4h            → "2h ago"
//   same day, ≥ 4h  → "14:30"
//   yesterday       → "Yesterday, 14:30"
//   < 7 days        → "Mon, 14:30"
//   this year       → "3 Jun"
//   older           → "3 Jun 2025"
// `hhmm_fallback` (TZ-free "HH:MM" from the ANCS Date) is used when the device
// clock isn't synced, so we never render a bogus relative time. 24-hour clock
// to match the status bar / clock screen.
static void format_notif_time(time_t when, time_t now,
                              const char* hhmm_fallback,
                              char* out, size_t sz) {
    out[0] = '\0';
    bool clock_ok = now > 1600000000;   // ~2020-09 — device clock has been synced
    if (when <= 0 || !clock_ok) {
        if (hhmm_fallback && hhmm_fallback[0]) snprintf(out, sz, "%s", hhmm_fallback);
        return;
    }
    long delta = (long)(now - when);
    if (delta < 0) delta = 0;

    if (delta < 60)       { snprintf(out, sz, "now");               return; }
    if (delta < 3600)     { snprintf(out, sz, "%ldm ago", delta / 60);   return; }
    if (delta < 4 * 3600) { snprintf(out, sz, "%ldh ago", delta / 3600); return; }

    struct tm w, n;
    localtime_r(&when, &w);
    localtime_r(&now,  &n);
    long day_diff = days_from_civil(n.tm_year + 1900, n.tm_mon + 1, n.tm_mday)
                  - days_from_civil(w.tm_year + 1900, w.tm_mon + 1, w.tm_mday);

    if (day_diff <= 0) {                       // earlier today (≥ 4h ago)
        snprintf(out, sz, "%02d:%02d", w.tm_hour, w.tm_min);
    } else if (day_diff == 1) {
        snprintf(out, sz, "Yesterday, %02d:%02d", w.tm_hour, w.tm_min);
    } else if (day_diff < 7) {
        snprintf(out, sz, "%s, %02d:%02d", kWeekday[w.tm_wday], w.tm_hour, w.tm_min);
    } else if (w.tm_year == n.tm_year) {
        snprintf(out, sz, "%d %s", w.tm_mday, kMonth[w.tm_mon]);
    } else {
        snprintf(out, sz, "%d %s %d", w.tm_mday, kMonth[w.tm_mon], w.tm_year + 1900);
    }
}

} // namespace

// Fill the shared view from a detail entry, formatting the timestamp now so
// it's fresh each time the modal opens.
static const AncsNotification& fill_detail_view(AncsDetailEntry* d) {
    format_notif_time(d->recv_epoch, time(nullptr), d->hhmm,
                      d->time_ago, sizeof(d->time_ago));
    k_detail_view.display_name = d->display_name;
    k_detail_view.title        = d->title[0] ? d->title : "Notification";
    k_detail_view.subtitle     = d->subtitle;  // "" → modal hides the line
    k_detail_view.body         = d->body[0]  ? d->body  : "No preview available.";
    k_detail_view.time_ago     = d->time_ago;  // "" → modal hides the line
    return k_detail_view;
}

const AncsNotification& ancs_notification(const char* token) {
    AncsDetailEntry* d = newest_detail_for_token(token);
    return d ? fill_detail_view(d) : k_ancs_fallback;
}

const AncsNotification& ancs_notification_by_uid(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) return fill_detail_view(&e);
    return k_ancs_fallback;
}

const Media& media()                  { return k_media; }
bool         media_playing()          { return k_playing; }
void         set_media_playing(bool playing) { k_playing = playing; }
void         set_media_volume(int v)  { if (v < 0) v = 0; if (v > 100) v = 100; k_media.volume = v; }
void         set_media_meta(const char* title, const char* artist, bool can_seek) {
    if (title)  strncpy(k_media_title,  title,  sizeof(k_media_title)  - 1);
    if (artist) strncpy(k_media_artist, artist, sizeof(k_media_artist) - 1);
    k_media.has_media = (title && title[0] != '\0');
    k_media.can_seek  = can_seek;
}
void         set_media_seek(uint32_t position_s, uint32_t duration_s) {
    k_media.position_s = position_s;
    k_media.duration_s = duration_s;
}
const ShortcutSlot* shortcuts() { return k_shortcuts; }

} // namespace app_state
