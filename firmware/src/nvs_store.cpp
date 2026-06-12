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
constexpr const char* k_mode        = "mode";       // uint8: 0=Calendar 1=Controls
constexpr const char* k_ota_ack     = "ota_ack";    // string: post-update version

Preferences prefs;

// Flags cached at init() so they are safe to read from any context
// (tick_cb → evaluate → compute_target_state) without opening NVS.
bool g_is_first_boot   = true;
bool g_awaiting_sync   = false;
bool g_awaiting_phone  = false;

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

bool is_awaiting_phone_pairing() {
    return g_awaiting_phone;
}

// ── Mode toggle persistence ────────────────────────────────────────────────

uint8_t get_mode() {
    uint8_t v = 0;  // default: Calendar
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        v = (uint8_t)prefs.getUChar(k_mode, 0);
        prefs.end();
    }
    return v;
}

void set_mode(uint8_t mode) {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putUChar(k_mode, mode);
        prefs.end();
    }
    LOG("[nvs] mode=%d\n", (int)mode);
}

// ── Factory reset ──────────────────────────────────────────────────────────

void factory_reset() {
    g_is_first_boot  = true;
    g_awaiting_sync  = false;
    g_awaiting_phone = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    LOG("[nvs] factory reset wipe complete\n");
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
