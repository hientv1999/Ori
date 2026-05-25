#include "nvs_store.h"

#include <Arduino.h>
#include <Preferences.h>

namespace {

constexpr const char* NAMESPACE = "ori";

// Key layout (see nvs_store.h for canonical table).
constexpr const char* k_backlight   = "bl";    // bool: backlight on/off
constexpr const char* k_provisioned = "prov";  // bool: setup completed
constexpr const char* k_mode        = "mode";  // uint8: 0=Calendar 1=Controls

// Backlight-toggle debounce per memory.md (~2 s). Rapid swipes don't write
// to flash repeatedly; only the last value within the window is committed.
constexpr uint32_t BACKLIGHT_DEBOUNCE_MS = 2000;

Preferences prefs;

bool     pending_backlight_dirty = false;
bool     pending_backlight_value = true;
uint32_t pending_backlight_deadline_ms = 0;

void flush_backlight() {
    if (!pending_backlight_dirty) return;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_backlight, pending_backlight_value);
        prefs.end();
    }
    pending_backlight_dirty = false;
    Serial.printf("[nvs] flushed backlight=%s\n",
                  pending_backlight_value ? "on" : "off");
}

} // namespace

namespace nvs {

void init() {
    // Open read-write so the namespace is created if it doesn't exist yet
    // (fresh flash, post-factory-reset). Read-only would return NOT_FOUND
    // on first boot and leave every subsequent prefs.begin() failing too.
    if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        Serial.println("[nvs] ERROR: could not open namespace — NVS partition missing?");
        return;
    }
    prefs.end();
    Serial.println("[nvs] ready");
}

void tick() {
    if (!pending_backlight_dirty) return;
    if ((int32_t)(millis() - pending_backlight_deadline_ms) >= 0) {
        flush_backlight();
    }
}

// ── Backlight (owned by backlight.cpp) ────────────────────────────────────

bool load_backlight_on(bool fallback) {
    bool v = fallback;
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        v = prefs.getBool(k_backlight, fallback);
        prefs.end();
    }
    return v;
}

void save_backlight_on(bool on) {
    pending_backlight_value         = on;
    pending_backlight_dirty         = true;
    pending_backlight_deadline_ms   = millis() + BACKLIGHT_DEBOUNCE_MS;
}

// ── First-boot detection ───────────────────────────────────────────────────

bool is_first_boot() {
    bool provisioned = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        provisioned = prefs.getBool(k_provisioned, false);
        prefs.end();
    }
    return !provisioned;
}

void mark_setup_complete() {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_provisioned, true);
        prefs.end();
    }
    Serial.println("[nvs] setup complete — provisioned flag set");
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
    Serial.printf("[nvs] mode=%d\n", (int)mode);
}

// ── Factory reset ──────────────────────────────────────────────────────────

void factory_reset() {
    // Drop any pending debounced write so it can't resurrect a stale value.
    pending_backlight_dirty = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    Serial.println("[nvs] factory reset wipe complete");
}

} // namespace nvs
