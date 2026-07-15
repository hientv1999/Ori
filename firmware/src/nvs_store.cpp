#include "nvs_store.h"

#include <Arduino.h>
#include "ori_log.h"
#include <Preferences.h>

namespace {

constexpr const char* NAMESPACE = "ori";

// Key layout (see nvs_store.h for canonical table).
constexpr const char* k_provisioned = "prov";       // bool: setup completed
constexpr const char* k_sync_step   = "sync_step";  // bool: Orion bonded, awaiting first sync
constexpr const char* k_phone_step  = "phone_step"; // bool: Orion synced, awaiting iPhone pair
constexpr const char* k_mode         = "mode";        // uint8: 0=Calendar 1=Controls
constexpr const char* k_clock_face   = "clock_face";  // uint8: 0=Digital 1=Analog
constexpr const char* k_time_format  = "time_fmt";    // uint8: 0=24-hour 1=12-hour
constexpr const char* k_notif_filter = "notif_filt";  // uint8: 0=Disabled 1=CallOnly 2=Important 3=All
constexpr const char* k_slot1        = "sc_1";         // string: shortcut slot 1 token
constexpr const char* k_slot2        = "sc_2";         // string: shortcut slot 2 token
constexpr const char* k_slot3        = "sc_3";         // string: shortcut slot 3 token
constexpr const char* k_seek_step    = "seek_step";   // uint8: double-tap seek step, seconds
constexpr const char* k_ota_ack      = "ota_ack";     // string: post-update version

Preferences prefs;

// Flags cached at init() so they are safe to read from any context
// (tick_cb → evaluate → compute_target_state) without opening NVS.
bool g_is_first_boot   = true;
bool g_awaiting_sync   = false;
bool g_awaiting_phone  = false;

// Shared body for the small uint8 preferences below (mode, clock_face,
// time_format, notif_filter): open read-only, read with the same default
// used on a missing key, close. Mirrors each call site's original
// prefs.begin/getUChar/end sequence exactly — only the key/default vary.
uint8_t get_uchar(const char* key, uint8_t dflt) {
    uint8_t v = dflt;
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        v = (uint8_t)prefs.getUChar(key, dflt);
        prefs.end();
    }
    return v;
}

// Shared body for the small uint8 preferences below: open read-write, write,
// close, then log. Logs unconditionally after the attempt, same as every
// original setter (success or failure of prefs.begin() was never itself
// logged). `%d` matches every original format string's printed output for a
// uint8_t value (0-255 always prints identically as %d or %u).
void set_uchar(const char* key, uint8_t val, const char* log_name) {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putUChar(key, val);
        prefs.end();
    }
    LOG("[nvs] %s=%d\n", log_name, (int)val);
}

} // namespace

namespace nvs {

void init() {
    // Open read-write so the namespace is created if it doesn't exist yet
    // (fresh flash, post-factory-reset), then read the provisioned flag once.
    if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        LOG("[nvs] ERROR: could not open namespace — NVS partition missing?\n");
        return;
    }
    g_is_first_boot  = !prefs.getBool(k_provisioned, false);
    g_awaiting_sync  =  prefs.getBool(k_sync_step,   false);
    g_awaiting_phone =  prefs.getBool(k_phone_step,  false);
    prefs.end();
    LOG("[nvs] ready  first_boot=%d  awaiting_sync=%d  awaiting_phone=%d\n",
        (int)g_is_first_boot, (int)g_awaiting_sync, (int)g_awaiting_phone);
}

void tick() {
    // Reserved for future debounced writes.
}

// ── First-boot detection ───────────────────────────────────────────────────

bool is_first_boot() {
    // Returns the value cached at init() — never reads NVS.
    // Safe to call from any context, including lv_timer_handler().
    return g_is_first_boot;
}

void mark_setup_complete() {
    g_is_first_boot  = false;
    g_awaiting_sync  = false;
    g_awaiting_phone = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_provisioned, true);
        // Clear both resume bookmarks so NVS matches the RAM flags. Harmless
        // either way (the resume path only runs while first_boot is true), but
        // keeps the persisted state honest.
        prefs.putBool(k_sync_step,  false);
        prefs.putBool(k_phone_step, false);
        prefs.end();
    }
    LOG("[nvs] setup complete — provisioned flag set\n");
}

void mark_orion_bonded() {
    g_awaiting_sync = true;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_sync_step, true);
        prefs.end();
    }
    LOG("[nvs] orion bonded — sync step persisted\n");
}

void mark_orion_synced() {
    // Advanced past the sync step: clear the sync bookmark, set the phone one.
    g_awaiting_sync  = false;
    g_awaiting_phone = true;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_sync_step,  false);
        prefs.putBool(k_phone_step, true);
        prefs.end();
    }
    LOG("[nvs] orion synced — phone pairing step persisted\n");
}

bool is_awaiting_sync() {
    return g_awaiting_sync;
}

void clear_orion_bonded() {
    // Mirrors mark_orion_bonded(), but reverting rather than setting — only
    // touches the sync-step bookmark. Leaves k_provisioned/g_is_first_boot
    // alone (still mid first-boot setup) and k_phone_step/g_awaiting_phone
    // alone (already false here — mark_orion_synced() is the only thing that
    // sets it, and by construction this path only runs when that never ran).
    g_awaiting_sync = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_sync_step, false);
        prefs.end();
    }
    LOG("[nvs] orion bond reverted — sync step bookmark cleared\n");
}

bool is_awaiting_phone_pairing() {
    return g_awaiting_phone;
}

// ── Mode toggle persistence ────────────────────────────────────────────────

uint8_t get_mode() {
    return get_uchar(k_mode, 0);  // default: Calendar
}

void set_mode(uint8_t mode) {
    set_uchar(k_mode, mode, "mode");
}

// ── Clock-face preference ──────────────────────────────────────────────────

uint8_t get_clock_face() {
    return get_uchar(k_clock_face, 0);  // default: Digital
}

void set_clock_face(uint8_t face) {
    set_uchar(k_clock_face, face, "clock_face");
}

// ── Time format preference (0 = 24-hour, 1 = 12-hour) ──────────────────────

uint8_t get_time_format() {
    return get_uchar(k_time_format, 0);  // default: 24-hour
}

void set_time_format(uint8_t fmt) {
    set_uchar(k_time_format, fmt, "time_format");
}

// ── ANCS notification filter ───────────────────────────────────────────────

uint8_t get_notif_filter() {
    return get_uchar(k_notif_filter, 0x03);  // default: All
}

void set_notif_filter(uint8_t level) {
    set_uchar(k_notif_filter, level, "notif_filter");
}

// ── Double-tap seek step (seconds) ──────────────────────────────────────────

uint8_t get_seek_step_s() {
    return get_uchar(k_seek_step, 10);  // default: 10 s
}

void set_seek_step_s(uint8_t seconds) {
    set_uchar(k_seek_step, seconds, "seek_step_s");
}

// ── Shortcut slot tokens ───────────────────────────────────────────────────

void get_shortcut_slots(char* s1, char* s2, char* s3, size_t slot_sz) {
    if (!prefs.begin(NAMESPACE, /*readOnly=*/true)) return;
    String v1 = prefs.getString(k_slot1, "vol-mute");
    String v2 = prefs.getString(k_slot2, "mic-mute");
    String v3 = prefs.getString(k_slot3, "screenshot");
    prefs.end();
    if (s1 && slot_sz > 0) { strncpy(s1, v1.c_str(), slot_sz - 1); s1[slot_sz - 1] = '\0'; }
    if (s2 && slot_sz > 0) { strncpy(s2, v2.c_str(), slot_sz - 1); s2[slot_sz - 1] = '\0'; }
    if (s3 && slot_sz > 0) { strncpy(s3, v3.c_str(), slot_sz - 1); s3[slot_sz - 1] = '\0'; }
}

void set_shortcut_slots(const char* s1, const char* s2, const char* s3) {
    if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) return;
    if (s1) prefs.putString(k_slot1, s1);
    if (s2) prefs.putString(k_slot2, s2);
    if (s3) prefs.putString(k_slot3, s3);
    prefs.end();
    LOG("[nvs] shortcut_slots=%s,%s,%s\n",
        s1 ? s1 : "", s2 ? s2 : "", s3 ? s3 : "");
}

// ── Factory reset ──────────────────────────────────────────────────────────

void factory_reset() {
    g_is_first_boot  = true;
    g_awaiting_sync  = false;
    g_awaiting_phone = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
        LOG("[nvs] factory reset wipe complete\n");
    } else {
        LOG("[nvs] ERROR: factory reset wipe FAILED — could not open namespace\n");
    }
}

// ── Post-OTA acknowledgement ────────────────────────────────────────────────

void set_ota_ack(const char* version) {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putString(k_ota_ack, version ? version : "");
        prefs.end();
    }
    LOG("[nvs] ota ack pending: %s\n", version ? version : "");
}

bool get_ota_ack(char* buf, uint32_t len) {
    bool present = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        if (prefs.isKey(k_ota_ack)) {
            prefs.getString(k_ota_ack, buf, len);
            present = true;
        }
        prefs.end();
    }
    return present;
}

void clear_ota_ack() {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.remove(k_ota_ack);
        prefs.end();
    }
    LOG("[nvs] ota ack cleared\n");
}

} // namespace nvs
