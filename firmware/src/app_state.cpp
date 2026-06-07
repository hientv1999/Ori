#include "app_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_mac.h>

namespace app_state {

namespace {

// Generic fallback for the ANCS notification detail modal.
// Real title/body/time come from ANCS attribute requests (M5).
static const AncsNotification k_ancs_fallback = {
    "Notification", "Notification", "No preview available.", "Just now"
};

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
    static char buf[12] = {};
    if (buf[0]) return buf;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
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

void dismiss_ancs_notification(const char* token) {
    if (!token) return;
    for (size_t i = 0; i < k_ancs.count; ++i) {
        if (k_ancs.icons[i] && strcmp(k_ancs.icons[i], token) == 0) {
            for (size_t j = i; j + 1 < k_ancs.count; ++j)
                k_ancs.icons[j] = k_ancs.icons[j + 1];
            k_ancs.icons[--k_ancs.count] = nullptr;
            return;
        }
    }
}

const AncsNotification& ancs_notification(const char* /*token*/) {
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
