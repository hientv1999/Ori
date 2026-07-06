#include "app_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include "nvs_store.h"

namespace app_state {

namespace {

// Generic fallback for the ANCS notification detail modal — used only before a
// notification's attributes have arrived. Empty time_ago so no fake time shows.
static const AncsNotification k_ancs_fallback = {
    "Notification", "Notification", "", "No preview available.", "", "", "", false, false
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
    uint8_t  category;        // AncsCategory::*
    bool     important;       // EventFlags Important bit
    char     pos_label[33];   // ANCS PositiveActionLabel; "" if absent
    char     neg_label[33];   // ANCS NegativeActionLabel; "" if absent
    bool     has_neg_action;  // EventFlags NEGATIVE_ACTION bit
    bool     silent;          // EventFlags SILENT bit
    uint32_t seq;             // insertion order — higher = more recent
    bool     used;
};
// Backing storage lives in PSRAM (allocated by app_state::init()) — at
// MAX_ANCS_NOTIFICATIONS=50 entries this is ~38 KB, which would otherwise sit
// in scarce internal SRAM for the life of the firmware. A thin span wrapper
// keeps every existing `for (auto& e : k_detail)` loop and `k_detail[i]`
// access unchanged.
struct DetailArray {
    AncsDetailEntry* data = nullptr;
    size_t           n    = 0;
    AncsDetailEntry*       begin()       { return data; }
    AncsDetailEntry*       end()         { return data + n; }
    AncsDetailEntry&       operator[](size_t i)       { return data[i]; }
};
static DetailArray      k_detail;
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

// Mutable shortcut token storage — updated by set_shortcuts() when Orion pushes
// a ShortcutConfig. Defaults match the mock_orion_ble.py defaults.
static char k_slot_tokens[SHORTCUT_COUNT][20] = { "vol-mute", "mic-mute", "screenshot" };
static ShortcutSlot k_shortcuts[SHORTCUT_COUNT] = {
    { k_slot_tokens[0] },
    { k_slot_tokens[1] },
    { k_slot_tokens[2] },
};

// Wall-clock time of the last successful BLE sync (SyncControl END).
// Zero until the first sync completes.
time_t g_last_sync_epoch = 0;

// Forward declaration — defined later in the second anonymous namespace block.
static void format_notif_time(time_t when, time_t now,
                              const char* hhmm_fallback,
                              char* out, size_t sz);

} // namespace

void init() {
    if (k_detail.data) return;  // idempotent
    k_detail.data = static_cast<AncsDetailEntry*>(
        heap_caps_calloc(MAX_ANCS_NOTIFICATIONS, sizeof(AncsDetailEntry),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (k_detail.data) {
        k_detail.n = MAX_ANCS_NOTIFICATIONS;
    } else {
        // PSRAM exhausted (shouldn't happen — this is ~38 KB out of 8 MB).
        // Fall back to a tiny SRAM store rather than crash on every ANCS event.
        static AncsDetailEntry s_fallback[4] = {};
        k_detail.data = s_fallback;
        k_detail.n = 4;
    }

    // Restore persisted shortcut slots so the first Device Settings read
    // (before Orion writes them) returns the last-saved assignment.
    nvs::get_shortcut_slots(k_slot_tokens[0], k_slot_tokens[1], k_slot_tokens[2],
                            sizeof(k_slot_tokens[0]));
}

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

// Set once Orion has established a true-UTC clock this session, with its POSIX TZ
// cached so the iPhone CTS fallback can stay on the same epoch basis (see header).
// RAM-only; never cleared except by reboot.
bool g_orion_clock_synced = false;
char g_orion_tz[64]       = {};

void set_orion_clock_synced(const char* tz) {
    g_orion_clock_synced = true;
    if (tz) { snprintf(g_orion_tz, sizeof(g_orion_tz), "%s", tz); }
}
bool        orion_clock_synced() { return g_orion_clock_synced; }
const char* orion_tz()           { return g_orion_tz; }

bool clock_is_set() {
    // Lower bound that rejects the ~1970 default after a cold boot but accepts
    // any real Orion-synced time. It MUST sit safely BELOW "now" — the old
    // 2026-07-12 floor was actually in the future, so a genuine sync (e.g. a
    // 2026-06 epoch) read as "unset" and the status-bar clock stayed blank.
    // 2025-01-01 00:00:00 UTC (epoch 1'735'689'600) is well past 1970 and below
    // any real deployment date.
    return time(nullptr) > 1735689600;
}

const char* synced_pill_text() {
    static char buf[48];
    if (g_last_sync_epoch == 0) return "LAST SYNCED";
    char t[24] = {};
    format_notif_time(g_last_sync_epoch, time(nullptr), "", t, sizeof(t));
    if (t[0]) snprintf(buf, sizeof(buf), "LAST SYNCED \xc2\xb7 %s", t);
    else       snprintf(buf, sizeof(buf), "LAST SYNCED");
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
                     const char* bundle, uint8_t category, bool important, bool silent,
                     const char* pos_label, const char* neg_label, bool neg_action) {
    // Reuse the slot for this uid (Modified), else a free slot, else evict the
    // oldest (lowest seq) so a full queue keeps the most recent detail.
    AncsDetailEntry* slot = nullptr;
    for (auto& e : k_detail) if (e.used && e.uid == uid) { slot = &e; break; }
    if (!slot) for (auto& e : k_detail) if (!e.used) { slot = &e; break; }
    if (!slot) { slot = &k_detail[0]; for (auto& e : k_detail) if (e.seq < slot->seq) slot = &e; }

    slot->used           = true;
    slot->uid            = uid;
    slot->seq            = ++k_detail_seq;
    slot->recv_epoch     = recv_epoch;
    slot->category       = category;
    slot->important      = important;
    slot->silent         = silent;
    slot->has_neg_action = neg_action;
    snprintf(slot->token,        sizeof(slot->token),        "%s", token ? token : "unknown");
    snprintf(slot->bundle,       sizeof(slot->bundle),       "%s", bundle ? bundle : "");
    snprintf(slot->display_name, sizeof(slot->display_name), "%s",
             (display_name && display_name[0]) ? display_name : "Notification");
    snprintf(slot->title,        sizeof(slot->title),        "%s", title     ? title     : "");
    snprintf(slot->subtitle,     sizeof(slot->subtitle),     "%s", subtitle  ? subtitle  : "");
    snprintf(slot->body,         sizeof(slot->body),         "%s", body      ? body      : "");
    snprintf(slot->hhmm,         sizeof(slot->hhmm),         "%s", hhmm      ? hhmm      : "");
    snprintf(slot->pos_label,    sizeof(slot->pos_label),    "%s", pos_label ? pos_label : "");
    snprintf(slot->neg_label,    sizeof(slot->neg_label),    "%s", neg_label ? neg_label : "");
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

time_t ancs_recv_epoch(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) return e.recv_epoch;
    return 0;
}

const char* ancs_title(uint32_t uid) {
    for (auto& e : k_detail) if (e.used && e.uid == uid) return e.title;
    return "";
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
    k_detail_view.display_name   = d->display_name;
    k_detail_view.title          = d->title[0] ? d->title : "Notification";
    k_detail_view.subtitle       = d->subtitle;  // "" → modal hides the line
    k_detail_view.body           = d->body[0]  ? d->body  : "No preview available.";
    k_detail_view.time_ago       = d->time_ago;  // "" → modal hides the line
    k_detail_view.pos_label      = d->pos_label;
    k_detail_view.neg_label      = d->neg_label;
    k_detail_view.has_neg_action = d->has_neg_action;
    k_detail_view.silent         = d->silent;
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

size_t ancs_collect_same_title(uint32_t uid, uint32_t* out_uids, size_t max) {
    if (!out_uids || max == 0) return 0;

    // Find the reference entry whose (token, title) defines the group.
    AncsDetailEntry* ref = nullptr;
    for (auto& e : k_detail) if (e.used && e.uid == uid) { ref = &e; break; }
    if (!ref) return 0;

    // Gather every live entry sharing the reference's app token AND title.
    AncsDetailEntry* matches[MAX_ANCS_NOTIFICATIONS];
    size_t n = 0;
    for (auto& e : k_detail) {
        if (!e.used) continue;
        if (strcmp(e.token, ref->token) != 0) continue;
        if (strcmp(e.title, ref->title) != 0) continue;
        if (n < MAX_ANCS_NOTIFICATIONS) matches[n++] = &e;
    }

    // Newest-first by the phone's notification time (recv_epoch), with arrival
    // seq as the tiebreaker for equal/unknown timestamps. Ordering by the real
    // time — not Ori's arrival order — keeps the chronology correct even when
    // iOS replays the notification backlog out of order on reconnect. The
    // overlay renders this reversed, so the OLDEST ends up at the top.
    // Selection sort; n ≤ 20.
    for (size_t i = 0; i < n; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < n; ++j) {
            bool newer = matches[j]->recv_epoch >  matches[best]->recv_epoch ||
                        (matches[j]->recv_epoch == matches[best]->recv_epoch &&
                         matches[j]->seq        >  matches[best]->seq);
            if (newer) best = j;
        }
        if (best != i) { AncsDetailEntry* t = matches[i]; matches[i] = matches[best]; matches[best] = t; }
    }

    size_t out_n = n < max ? n : max;
    for (size_t i = 0; i < out_n; ++i) out_uids[i] = matches[i]->uid;
    return out_n;
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

void set_shortcuts(const char* s1, const char* s2, const char* s3) {
    strncpy(k_slot_tokens[0], s1 ? s1 : "", sizeof(k_slot_tokens[0]) - 1);
    strncpy(k_slot_tokens[1], s2 ? s2 : "", sizeof(k_slot_tokens[1]) - 1);
    strncpy(k_slot_tokens[2], s3 ? s3 : "", sizeof(k_slot_tokens[2]) - 1);
}

} // namespace app_state
